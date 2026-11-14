#include <nuvix/kiov.h>
#include <nuvix/path.h>
#include <nuvix/vfs.h>
#include <nuvix/errno.h>
#include <nuvix/refcount.h>
#include <nuvix/slab.h>
#include <nuvix/mutex.h>
#include <nuvix/macro.h>
#include <nuvix/math.h>
#include <nuvix/string.h>
#include <nuvix/syscall.h>
#include <nuvix/fdtable.h>
#include <nuvix/bitmap.h>
#include <nuvix/uaccess.h>
#include <asm/trap.h>

struct file *vfs_open(int dirfd, const char *path, unsigned long flags)
{
    struct path resolved = {0};
    struct file *file __free(kfree) = NULL;
    unsigned int access = flags & O_ACCMODE;
    unsigned int lookup = flags & O_NOFOLLOW ? 0 : PATH_FOLLOW;
    struct inode *inode;
    int ret;

    if (!*path)
        return ERR_PTR(-ENOENT);
    if (flags & ~(O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_NONBLOCK |
                  O_LARGEFILE | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC))
        return ERR_PTR(-EOPNOTSUPP);
    if (access == O_ACCMODE)
        return ERR_PTR(-EINVAL);
    if ((flags & (O_CREAT | O_DIRECTORY)) == (O_CREAT | O_DIRECTORY))
        return ERR_PTR(-EINVAL);
    if (flags & O_CREAT) {
        lookup |= PATH_CREATE;
        if (flags & O_EXCL)
            lookup |= PATH_EXCL;
    }

    ret = vfs_lookup(dirfd, path, lookup, &resolved);
    if (ret)
        return ERR_PTR(ret);

    inode = resolved.dentry->d_inode;
    if (!inode) {
        ret = -ENOENT;
        goto fail;
    }
    if ((flags & O_DIRECTORY) && !S_ISDIR(inode->i_mode)) {
        ret = -ENOTDIR;
        goto fail;
    }
    if (S_ISLNK(inode->i_mode)) {
        ret = -ELOOP;
        goto fail;
    }
    if (!inode->i_fops) {
        ret = -EOPNOTSUPP;
        goto fail;
    }
    if (S_ISDIR(inode->i_mode) &&
        (access != O_RDONLY || (flags & (O_CREAT | O_TRUNC)))) {
        ret = (flags & O_CREAT) && (flags & O_EXCL) ? -EEXIST : -EISDIR;
        goto fail;
    }

    file = kzalloc(sizeof(*file));
    if (!file) {
        ret = -ENOMEM;
        goto fail;
    }

    refcount_set(&file->f_count, 1);
    mutex_init(&file->f_pos_lock);
    file->f_flags = flags & ~(O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC);
    file->f_pos = 0;
    file->f_inode = inode;
    file->f_ops = inode->i_fops;
    file->f_path = move(resolved);

    if (file->f_ops->open) {
        ret = file->f_ops->open(file);
        if (ret) {
            path_put(&file->f_path);
            return ERR_PTR(ret);
        }
    }
    if ((flags & O_TRUNC) && S_ISREG(inode->i_mode)) {
        struct iattr attr = {.ia_valid = ATTR_SIZE, .ia_size = 0};

        ret = inode->i_ops->setattr ? inode->i_ops->setattr(inode, &attr)
                                    : -EOPNOTSUPP;
        if (ret) {
            fput(move(file));
            return ERR_PTR(ret);
        }
    }

    return move(file);

fail:
    path_put(&resolved);
    return ERR_PTR(ret);
}

ssize_t vfs_read(struct kiocb *iocb, struct kiov *iov)
{
    if (!iocb->file->f_ops->read)
        return -EINVAL;
    if (!iov->count)
        return 0;
    return iocb->file->f_ops->read(iocb, iov);
}

ssize_t vfs_write(struct kiocb *iocb, struct kiov *iov)
{
    if (!iocb->file->f_ops->write)
        return -EINVAL;
    if (!iov->count)
        return 0;
    return iocb->file->f_ops->write(iocb, iov);
}

struct file *fget(struct file *file)
{
    if (file)
        refcount_inc(&file->f_count);
    return file;
}

int fput(struct file *file)
{
    int ret = 0;

    if (!file)
        return 0;
    if (!refcount_dec_and_test(&file->f_count))
        return 0;
    if (file->f_ops && file->f_ops->release)
        ret = file->f_ops->release(file);
    path_put(&file->f_path);
    kfree(file);
    return ret;
}

long sys_openat(struct trap_frame *frame)
{
    int dirfd = (int)syscallarg(frame, 0);
    const char *upath = (const char *)syscallarg(frame, 1);
    unsigned long flags = syscallarg(frame, 2) & ~O_LARGEFILE;
    bool cloexec = flags & O_CLOEXEC;
    char *path __free(kfree) = get_pathname(upath);
    struct file *file __free(fput) = NULL;
    int fd;

    if (IS_ERR(path))
        return PTR_ERR(move(path));
    fd = fd_alloc(current->fdtable);
    if (fd < 0)
        return fd;
    file = vfs_open(dirfd, path, flags);
    if (IS_ERR(file)) {
        fd_release(current->fdtable, fd);
        return PTR_ERR(move(file));
    }
    fd_replace(current->fdtable, (unsigned long)fd, move(file), cloexec);
    return fd;
}

char *get_pathname(const char *upath)
{
    char *path __free(kfree) = kmalloc(PATH_MAX);
    ssize_t len;

    if (!path)
        return ERR_PTR(-ENOMEM);
    len = strncpy_from_user(path, upath, PATH_MAX);
    if (len < 0)
        return ERR_PTR(len);
    if (len == PATH_MAX)
        return ERR_PTR(-ENAMETOOLONG);
    return move(path);
}

static struct dentry *start_creating(int dirfd, const char *path,
                                     bool directory, struct path *parent)
{
    struct path target = {0};
    struct inode *dir;
    const char *last;
    char name[VFS_NAME_MAX + 1];
    size_t len;
    int ret;

    ret = vfs_lookparent(dirfd, path, parent, &last, &len);
    if (ret)
        return ERR_PTR(ret);
    if (!len || (len == 1 && last[0] == '.') ||
        (len == 2 && last[0] == '.' && last[1] == '.')) {
        ret = -EEXIST;
        goto out;
    }
    memcpy(name, last, len);
    name[len] = '\0';
    dir = parent->dentry->d_inode;
    mutex_lock(&dir->mutex);
    ret = vfs_lookone(parent, name, &target);
    if (ret)
        goto unlock;
    if (target.dentry->d_inode) {
        ret = -EEXIST;
        goto unlock;
    }
    if (!directory && last[len] == '/') {
        ret = -ENOENT;
        goto unlock;
    }
    return target.dentry;

unlock:
    mutex_unlock(&dir->mutex);
    path_put(&target);
out:
    path_put(parent);
    return ERR_PTR(ret);
}

static void end_creating(struct path *parent, struct dentry *dentry)
{
    mutex_unlock(&parent->dentry->d_inode->mutex);
    dput(dentry);
    path_put(parent);
}

long sys_mkdirat(struct trap_frame *frame)
{
    int dirfd = (int)syscallarg(frame, 0);
    const char *upath = (const char *)syscallarg(frame, 1);
    char *path __free(kfree) = get_pathname(upath);
    struct path parent = {0};
    struct dentry *dentry;
    struct dentry *created;
    struct inode *dir;
    int ret;

    if (IS_ERR(path))
        return PTR_ERR(move(path));
    dentry = start_creating(dirfd, path, true, &parent);
    if (IS_ERR(dentry))
        return PTR_ERR(dentry);
    dir = parent.dentry->d_inode;
    if (!dir->i_ops->mkdir) {
        ret = -EPERM;
    } else {
        created = dir->i_ops->mkdir(dir, dentry);
        ret = IS_ERR(created) ? PTR_ERR(created) : 0;
    }
    end_creating(&parent, dentry);
    return ret;
}

long sys_unlinkat(struct trap_frame *frame)
{
    int dirfd = (int)syscallarg(frame, 0);
    const char *upath = (const char *)syscallarg(frame, 1);
    unsigned int flags = syscallarg(frame, 2);
    bool directory = flags & AT_REMOVEDIR;
    char *path __free(kfree) = NULL;
    struct path parent = {0};
    struct path target = {0};
    struct inode *dir;
    struct inode *inode;
    const char *last;
    char name[VFS_NAME_MAX + 1];
    size_t len;
    int ret;

    if (flags & ~AT_REMOVEDIR)
        return -EINVAL;
    path = get_pathname(upath);
    if (IS_ERR(path))
        return PTR_ERR(move(path));
    ret = vfs_lookparent(dirfd, path, &parent, &last, &len);
    if (ret)
        return ret;
    if (!len) {
        ret = directory ? -EBUSY : -EISDIR;
        goto out;
    }
    if ((len == 1 && last[0] == '.') ||
        (len == 2 && last[0] == '.' && last[1] == '.')) {
        ret = directory ? (len == 1 ? -EINVAL : -ENOTEMPTY) : -EISDIR;
        goto out;
    }
    memcpy(name, last, len);
    name[len] = '\0';
    dir = parent.dentry->d_inode;
    mutex_lock(&dir->mutex);
    ret = vfs_lookone(&parent, name, &target);
    if (ret)
        goto unlock;
    inode = target.dentry->d_inode;
    if (!inode) {
        ret = -ENOENT;
        goto unlock;
    }
    if (directory && !S_ISDIR(inode->i_mode)) {
        ret = -ENOTDIR;
        goto unlock;
    }
    if (!directory && (S_ISDIR(inode->i_mode) || last[len] == '/')) {
        ret = S_ISDIR(inode->i_mode) ? -EISDIR : -ENOTDIR;
        goto unlock;
    }
    mutex_lock(&inode->mutex);
    if (directory)
        ret =
            dir->i_ops->rmdir ? dir->i_ops->rmdir(dir, target.dentry) : -EPERM;
    else
        ret = dir->i_ops->unlink ? dir->i_ops->unlink(dir, target.dentry)
                                 : -EPERM;
    if (!ret)
        dcache_drop(target.dentry);
    mutex_unlock(&inode->mutex);
unlock:
    mutex_unlock(&dir->mutex);
out:
    path_put(&target);
    path_put(&parent);
    return ret;
}

long sys_symlinkat(struct trap_frame *frame)
{
    const char *utarget = (const char *)syscallarg(frame, 0);
    int dirfd = (int)syscallarg(frame, 1);
    const char *upath = (const char *)syscallarg(frame, 2);
    char *target __free(kfree) = get_pathname(utarget);
    char *path __free(kfree) = NULL;
    struct path parent = {0};
    struct dentry *dentry;
    struct inode *dir;
    int ret;

    if (IS_ERR(target))
        return PTR_ERR(move(target));
    if (!*target)
        return -ENOENT;
    path = get_pathname(upath);
    if (IS_ERR(path))
        return PTR_ERR(move(path));
    dentry = start_creating(dirfd, path, false, &parent);
    if (IS_ERR(dentry))
        return PTR_ERR(dentry);
    dir = parent.dentry->d_inode;
    ret =
        dir->i_ops->symlink ? dir->i_ops->symlink(dir, dentry, target) : -EPERM;
    end_creating(&parent, dentry);
    return ret;
}

long sys_linkat(struct trap_frame *frame)
{
    int oldfd = (int)syscallarg(frame, 0);
    const char *uold = (const char *)syscallarg(frame, 1);
    int newfd = (int)syscallarg(frame, 2);
    const char *unew = (const char *)syscallarg(frame, 3);
    unsigned int flags = syscallarg(frame, 4);
    unsigned int lookup = 0;
    char *old __free(kfree) = NULL;
    char *new __free(kfree) = NULL;
    struct path source = {0};
    struct path parent = {0};
    struct dentry *dentry;
    struct inode *inode;
    struct inode *dir;
    int ret;

    if (flags & ~(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH))
        return -EINVAL;
    if (flags & AT_SYMLINK_FOLLOW)
        lookup |= PATH_FOLLOW;
    if (flags & AT_EMPTY_PATH)
        lookup |= PATH_EMPTY;
    old = get_pathname(uold);
    if (IS_ERR(old))
        return PTR_ERR(move(old));
    new = get_pathname(unew);
    if (IS_ERR(new))
        return PTR_ERR(move(new));
    ret = vfs_lookup(oldfd, old, lookup, &source);
    if (ret)
        return ret;
    dentry = start_creating(newfd, new, false, &parent);
    if (IS_ERR(dentry)) {
        ret = PTR_ERR(dentry);
        goto out;
    }
    inode = source.dentry->d_inode;
    dir = parent.dentry->d_inode;
    if (dir->i_sb != inode->i_sb) {
        ret = -EXDEV;
    } else if (S_ISDIR(inode->i_mode) || !dir->i_ops->link) {
        ret = -EPERM;
    } else {
        mutex_lock(&inode->mutex);
        ret = inode->i_nlink ? dir->i_ops->link(source.dentry, dir, dentry)
                             : -ENOENT;
        mutex_unlock(&inode->mutex);
    }
    end_creating(&parent, dentry);
out:
    path_put(&source);
    return ret;
}

struct renamedata {
    struct path parent;
    struct path entry;
    struct inode *dir;
    struct inode *inode;
    const char *name;
    bool slash;
};

static bool is_ancestor(const struct dentry *ancestor,
                        const struct dentry *dentry)
{
    for (; dentry; dentry = dentry->d_parent) {
        if (dentry == ancestor)
            return true;
    }
    return false;
}

static int resolve_lastname(int dirfd, char *path, struct renamedata *data)
{
    const char *last;
    size_t len;
    int ret;

    ret = vfs_lookparent(dirfd, path, &data->parent, &last, &len);
    if (ret)
        return ret;
    if (!len || (len == 1 && last[0] == '.') ||
        (len == 2 && last[0] == '.' && last[1] == '.'))
        return -EBUSY;
    data->slash = last[len] == '/';
    ((char *)last)[len] = '\0';
    data->name = last;
    return 0;
}

long sys_renameat2(struct trap_frame *frame)
{
    int oldfd = (int)syscallarg(frame, 0);
    const char *uold = (const char *)syscallarg(frame, 1);
    int newfd = (int)syscallarg(frame, 2);
    const char *unew = (const char *)syscallarg(frame, 3);
    unsigned int flags = syscallarg(frame, 4);
    char *oldpath __free(kfree) = NULL;
    char *newpath __free(kfree) = NULL;
    struct renamedata old = {0};
    struct renamedata new = {0};
    struct inode *dead = NULL;
    int ret;

    if (flags & ~RENAME_NOREPLACE)
        return -EINVAL;
    oldpath = get_pathname(uold);
    if (IS_ERR(oldpath))
        return PTR_ERR(move(oldpath));
    newpath = get_pathname(unew);
    if (IS_ERR(newpath))
        return PTR_ERR(move(newpath));
    ret = resolve_lastname(oldfd, oldpath, &old);
    if (ret)
        goto out;
    ret = resolve_lastname(newfd, newpath, &new);
    if (ret)
        goto out;

    old.dir = old.parent.dentry->d_inode;
    new.dir = new.parent.dentry->d_inode;
    if (old.dir->i_sb != new.dir->i_sb) {
        ret = -EXDEV;
        goto out;
    }
    if (!old.dir->i_ops->rename) {
        ret = -EPERM;
        goto out;
    }

    if (old.dir == new.dir) {
        mutex_lock(&old.dir->mutex);
    } else if ((uintptr_t)old.dir < (uintptr_t)new.dir) {
        mutex_lock(&old.dir->mutex);
        mutex_lock(&new.dir->mutex);
    } else {
        mutex_lock(&new.dir->mutex);
        mutex_lock(&old.dir->mutex);
    }
    ret = vfs_lookone(&old.parent, old.name, &old.entry);
    if (ret)
        goto dirs;
    ret = vfs_lookone(&new.parent, new.name, &new.entry);
    if (ret)
        goto dirs;

    old.inode = old.entry.dentry->d_inode;
    new.inode = new.entry.dentry->d_inode;
    if (!old.inode) {
        ret = -ENOENT;
        goto dirs;
    }
    if (old.slash && !S_ISDIR(old.inode->i_mode)) {
        ret = -ENOTDIR;
        goto dirs;
    }
    if (S_ISDIR(old.inode->i_mode)) {
        if (new.inode && !S_ISDIR(new.inode->i_mode)) {
            ret = -ENOTDIR;
            goto dirs;
        }
        if (!new.inode && new.slash) {
            ret = -ENOENT;
            goto dirs;
        }
    } else {
        if (new.slash) {
            ret = -ENOTDIR;
            goto dirs;
        }
        if (new.inode && S_ISDIR(new.inode->i_mode)) {
            ret = -EISDIR;
            goto dirs;
        }
    }
    if (!old.entry.dentry->d_parent) {
        ret = -EBUSY;
        goto dirs;
    }

    if (is_ancestor(old.entry.dentry, new.parent.dentry)) {
        ret = -EINVAL;
        goto dirs;
    }
    if (new.entry.dentry != old.entry.dentry &&
        is_ancestor(new.entry.dentry, old.entry.dentry)) {
        ret = -ENOTEMPTY;
        goto dirs;
    }

    if (new.inode && (uintptr_t)new.inode < (uintptr_t)old.inode) {
        mutex_lock(&new.inode->mutex);
        mutex_lock(&old.inode->mutex);
    } else {
        mutex_lock(&old.inode->mutex);
        if (new.inode && new.inode != old.inode)
            mutex_lock(&new.inode->mutex);
    }
    ret = old.dir->i_ops->rename(old.dir, old.entry.dentry, new.dir,
                                 new.entry.dentry, flags);
    if (!ret && old.inode != new.inode) {
        dcache_drop(old.entry.dentry);
        ihold(old.inode);
        new.entry.dentry->d_inode = old.inode;
        dead = new.inode;
    }
    mutex_unlock(&old.inode->mutex);
    if (new.inode && new.inode != old.inode)
        mutex_unlock(&new.inode->mutex);
dirs:
    mutex_unlock(&old.dir->mutex);
    if (new.dir != old.dir)
        mutex_unlock(&new.dir->mutex);
    iput(dead);
out:
    path_put(&old.parent);
    path_put(&old.entry);
    path_put(&new.parent);
    path_put(&new.entry);
    return ret;
}

long sys_readlinkat(struct trap_frame *frame)
{
    int dirfd = (int)syscallarg(frame, 0);
    const char *upath = (const char *)syscallarg(frame, 1);
    char *ubuf = (char *)syscallarg(frame, 2);
    int bufsiz = (int)syscallarg(frame, 3);
    char *path __free(kfree) = NULL;
    char *buf __free(kfree) = NULL;
    struct path resolved = {0};
    struct inode *inode;
    ssize_t ret;

    if (bufsiz <= 0)
        return -EINVAL;
    path = get_pathname(upath);
    if (IS_ERR(path))
        return PTR_ERR(move(path));
    ret = vfs_lookup(dirfd, path, PATH_EMPTY, &resolved);
    if (ret)
        return ret;
    inode = resolved.dentry->d_inode;
    if (!S_ISLNK(inode->i_mode) || !inode->i_ops->readlink) {
        ret = *path ? -EINVAL : -ENOENT;
        goto out;
    }
    bufsiz = MIN(bufsiz, PATH_MAX);
    buf = kmalloc(bufsiz);
    if (!buf) {
        ret = -ENOMEM;
        goto out;
    }
    ret = inode->i_ops->readlink(inode, buf, bufsiz);
    if (ret >= 0 && copy_to_user(ubuf, buf, ret))
        ret = -EFAULT;
out:
    path_put(&resolved);
    return ret;
}

long sys_close(struct trap_frame *frame)
{
    unsigned int fd = syscallarg(frame, 0);

    return fd_close(current->fdtable, fd);
}

long sys_fcntl(struct trap_frame *frame)
{
    unsigned int fd = syscallarg(frame, 0);
    unsigned int cmd = syscallarg(frame, 1);
    unsigned long arg = syscallarg(frame, 2);
    struct fdtable *fdt = current->fdtable;
    struct file *file __free(fput) = fd_open(fdt, fd);

    if (!file)
        return -EBADF;

    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        int min = (int)arg;
        int newfd;

        if (min < 0 || (unsigned long)min >= BITS_PER_LONG)
            return -EINVAL;
        newfd = fd_alloc_from(fdt, (unsigned long)min);
        if (newfd < 0)
            return newfd;
        fd_replace(fdt, (unsigned long)newfd, move(file),
                   cmd == F_DUPFD_CLOEXEC);
        return newfd;
    }
    case F_GETFD:
        return fd_test_cloexec(fdt, fd) ? FD_CLOEXEC : 0;
    case F_SETFD:
        fd_set_cloexec(fdt, fd, arg & FD_CLOEXEC);
        return 0;
    case F_GETFL:
        return file->f_flags;
    case F_SETFL:
        file->f_flags = (file->f_flags & ~O_NONBLOCK) | (arg & O_NONBLOCK);
        return 0;
    default:
        return -EINVAL;
    }
}

long sys_chdir(struct trap_frame *frame)
{
    const char *upath = (const char *)syscallarg(frame, 0);
    struct path resolved = {0};
    char *path __free(kfree) = NULL;
    struct inode *inode;
    int ret;

    path = get_pathname(upath);
    if (IS_ERR(path))
        return PTR_ERR(move(path));

    ret = vfs_look(path, &resolved);
    if (ret)
        return ret;

    inode = resolved.dentry->d_inode;
    if (!inode) {
        ret = -ENOENT;
        goto fail;
    }
    if (!S_ISDIR(inode->i_mode)) {
        ret = -ENOTDIR;
        goto fail;
    }

    path_put(&current->cwd);
    current->cwd = move(resolved);
    return 0;

fail:
    path_put(&resolved);
    return ret;
}

long sys_fchdir(struct trap_frame *frame)
{
    unsigned int fd = syscallarg(frame, 0);
    struct file *file __free(fput) = fd_open(current->fdtable, fd);

    if (!file)
        return -EBADF;

    if (!S_ISDIR(file->f_inode->i_mode))
        return -ENOTDIR;

    path_put(&current->cwd);
    path_get(&file->f_path);
    current->cwd = file->f_path;
    return 0;
}

long sys_getcwd(struct trap_frame *frame)
{
    char *ubuf = (char *)syscallarg(frame, 0);
    size_t size = syscallarg(frame, 1);
    struct dentry *dentry = current->cwd.dentry;
    char *buffer __free(kfree) = NULL;
    char *pos;
    size_t len;

    if (!size)
        return -EINVAL;
    if (!dentry)
        return -ENOENT;

    buffer = kmalloc(PATH_MAX);
    if (!buffer)
        return -ENOMEM;

    pos = buffer + PATH_MAX;
    *--pos = '\0';
    for (; dentry->d_parent; dentry = dentry->d_parent) {
        size_t n = dentry->d_nlen;

        if ((size_t)(pos - buffer) < n + 1)
            return -ENAMETOOLONG;
        pos -= n;
        memcpy(pos, dentry->d_name, n);
        *--pos = '/';
    }
    if (current->cwd.dentry->d_parent == NULL)
        *--pos = '/';

    len = (size_t)(buffer + PATH_MAX - pos);
    if (len > size)
        return -ERANGE;
    if (copy_to_user(ubuf, pos, len))
        return -EFAULT;
    return (long)len;
}

static int fill_stat(struct inode *inode, void *ustat)
{
    struct kstat ks = {0};
    struct stat st;
    int ret;

    if (!inode->i_ops || !inode->i_ops->getattr)
        return -EOPNOTSUPP;
    ret = inode->i_ops->getattr(inode, &ks);
    if (ret)
        return ret;

    st = (struct stat){
        .st_dev = ks.dev,
        .st_ino = ks.ino,
        .st_mode = ks.mode,
        .st_nlink = ks.nlink,
        .st_uid = ks.uid,
        .st_gid = ks.gid,
        .st_rdev = ks.rdev,
        .st_size = ks.size,
        .st_blksize = ks.blksize,
        .st_blocks = ks.blocks,
        .st_atime = ks.atime.tv_sec,
        .st_atime_nsec = ks.atime.tv_nsec,
        .st_mtime = ks.mtime.tv_sec,
        .st_mtime_nsec = ks.mtime.tv_nsec,
        .st_ctime = ks.ctime.tv_sec,
        .st_ctime_nsec = ks.ctime.tv_nsec,
    };

    if (copy_to_user(ustat, &st, sizeof(st)))
        return -EFAULT;
    return 0;
}

long sys_newfstatat(struct trap_frame *frame)
{
    int dirfd = (int)syscallarg(frame, 0);
    const char *upath = (const char *)syscallarg(frame, 1);
    void *ustat = (void *)syscallarg(frame, 2);
    int flags = (int)syscallarg(frame, 3);
    struct path resolved = {0};
    char *path __free(kfree) = NULL;
    struct inode *inode;
    int ret;

    if (flags & ~AT_SYMLINK_NOFOLLOW)
        return -EOPNOTSUPP;

    path = get_pathname(upath);
    if (IS_ERR(path))
        return PTR_ERR(move(path));
    ret = vfs_lookup(dirfd, path, flags & AT_SYMLINK_NOFOLLOW ? 0 : PATH_FOLLOW,
                     &resolved);
    if (ret)
        return ret;

    inode = resolved.dentry->d_inode;
    if (!inode) {
        ret = -ENOENT;
        goto out;
    }

    ret = fill_stat(inode, ustat);

out:
    path_put(&resolved);
    return ret;
}

long sys_fstat(struct trap_frame *frame)
{
    unsigned int fd = syscallarg(frame, 0);
    void *ustat = (void *)syscallarg(frame, 1);
    struct file *file __free(fput) = fd_open(current->fdtable, fd);
    struct inode *inode;

    if (!file)
        return -EBADF;

    inode = file->f_inode;
    if (!inode)
        return -ENOENT;
    return fill_stat(inode, ustat);
}

long sys_ftruncate(struct trap_frame *frame)
{
    unsigned int fd = syscallarg(frame, 0);
    loff_t len = syscallarg(frame, 1);
    struct iattr attr = {.ia_valid = ATTR_SIZE, .ia_size = len};
    struct file *file __free(fput) = fd_open(current->fdtable, fd);
    struct inode *inode;

    if (len < 0)
        return -EINVAL;
    if (!file)
        return -EBADF;
    inode = file->f_inode;

    if (!S_ISREG(inode->i_mode) || !FILE_WRITE(file))
        return -EINVAL;

    if (!inode->i_ops->setattr)
        return -ENOTSUPP;

    return inode->i_ops->setattr(inode, &attr);
}

long sys_fsync(struct trap_frame *frame)
{
    unsigned int fd = syscallarg(frame, 0);
    struct file *file __free(fput) = fd_open(current->fdtable, fd);

    if (!file)
        return -EBADF;

    if (!file->f_ops->fsync)
        return -EINVAL;

    return file->f_ops->fsync(file);
}

long sys_lseek(struct trap_frame *frame)
{
    struct file *file __free(fput) =
        fd_open(current->fdtable, syscallarg(frame, 0));
    loff_t offset = (loff_t)syscallarg(frame, 1);
    int whence = (int)syscallarg(frame, 2);

    if (!file)
        return -EBADF;
    if (!file->f_ops->llseek)
        return -ESPIPE;
    return file->f_ops->llseek(file, offset, whence);
}

struct getdents_ctx {
    struct dirctx ctx;
    char *buf;
    size_t count;
    size_t done;
    int error;
};

static bool filldir64(struct dirctx *ctx, const char *name, int namelen,
                      loff_t offset, u64 ino, unsigned type)
{
    struct getdents_ctx *d = container_of(ctx, struct getdents_ctx, ctx);
    char buffer[ALIGN_UP(offsetof(struct dirent64, d_name) + VFS_NAME_MAX + 1,
                         8)];
    struct dirent64 *dirent = (struct dirent64 *)buffer;
    size_t reclen;

    if (namelen < 0 || namelen > VFS_NAME_MAX) {
        d->error = -EIO;
        return false;
    }

    reclen = ALIGN_UP(offsetof(struct dirent64, d_name) + namelen + 1, 8);
    if (reclen > d->count - d->done) {
        d->error = -EINVAL;
        return false;
    }

    dirent->d_ino = ino;
    dirent->d_off = offset;
    dirent->d_reclen = (uint16_t)reclen;
    dirent->d_type = (uint8_t)type;
    memcpy(dirent->d_name, name, namelen);
    memset(dirent->d_name + namelen, 0,
           reclen - offsetof(struct dirent64, d_name) - namelen);

    if (copy_to_user(d->buf + d->done, buffer, reclen)) {
        d->error = -EFAULT;
        return false;
    }

    d->done += reclen;
    return true;
}

long sys_getdents64(struct trap_frame *frame)
{
    unsigned int fd = syscallarg(frame, 0);
    char *ubuf = (char *)syscallarg(frame, 1);
    size_t count = syscallarg(frame, 2);
    struct file *file __free(fput) = fd_open(current->fdtable, fd);
    struct getdents_ctx d = {0};
    int ret;

    if (!file)
        return -EBADF;
    if (!file->f_ops->readdir)
        return -ENOTDIR;

    ret = mutex_lock_interruptible(&file->f_pos_lock);
    if (ret)
        return ret;

    d.ctx.actor = filldir64;
    d.ctx.pos = file->f_pos;
    d.buf = ubuf;
    d.count = count;

    ret = file->f_ops->readdir(file, &d.ctx);
    file->f_pos = d.ctx.pos;
    mutex_unlock(&file->f_pos_lock);

    /*
     * Copied entries win over any error: f_pos already moved past them, so
     * reporting the error instead of the byte count would lose them.
     */
    if (d.done)
        return (long)d.done;
    if (ret)
        return ret;
    return d.error;
}

long sys_ioctl(struct trap_frame *frame)
{
    unsigned int fd = syscallarg(frame, 0);
    unsigned int cmd = syscallarg(frame, 1);
    unsigned long arg = syscallarg(frame, 2);
    struct file *file __free(fput) = fd_open(current->fdtable, fd);

    if (!file)
        return -EBADF;
    if (!file->f_ops->ioctl)
        return -ENOTTY;
    return file->f_ops->ioctl(file, cmd, arg);
}

static ssize_t file_read(struct file *file, struct kiov *iov)
{
    struct kiocb iocb = {.file = file};
    ssize_t ret;

    if (!iov->count)
        return 0;
    ret = mutex_lock_interruptible(&file->f_pos_lock);
    if (ret)
        return ret;
    iocb.pos = file->f_pos;
    ret = vfs_read(&iocb, iov);
    if (ret > 0)
        file->f_pos = iocb.pos;
    mutex_unlock(&file->f_pos_lock);
    return ret;
}

static ssize_t file_write(struct file *file, struct kiov *iov)
{
    struct kiocb iocb = {.file = file};
    ssize_t ret;

    if (!iov->count)
        return 0;
    ret = mutex_lock_interruptible(&file->f_pos_lock);
    if (ret)
        return ret;
    iocb.pos = file->f_pos;
    ret = vfs_write(&iocb, iov);
    if (ret > 0)
        file->f_pos = iocb.pos;
    mutex_unlock(&file->f_pos_lock);
    return ret;
}

long sys_read(struct trap_frame *frame)
{
    unsigned int fd = syscallarg(frame, 0);
    void *buffer = (void *)syscallarg(frame, 1);
    size_t len = MIN(syscallarg(frame, 2), (size_t)SSIZE_MAX);
    struct file *file __free(fput) = fd_open(current->fdtable, fd);
    struct iovec vec = {.iov_base = buffer, .iov_len = len};
    struct kiov iov;

    if (!file || !FILE_READ(file))
        return -EBADF;
    kiov_init(&iov, &vec, 1, len, true);
    return file_read(file, &iov);
}

long sys_pread64(struct trap_frame *frame)
{
    unsigned int fd = syscallarg(frame, 0);
    void *buffer = (void *)syscallarg(frame, 1);
    size_t len = MIN(syscallarg(frame, 2), (size_t)SSIZE_MAX);
    loff_t offset = syscallarg(frame, 3);

    struct file *file __free(fput) = fd_open(current->fdtable, fd);
    struct kiocb iocb = {.file = file, .pos = offset};
    struct iovec vec = {.iov_base = buffer, .iov_len = len};
    struct kiov iov;

    if (offset < 0)
        return -EINVAL;
    if (!file)
        return -EBADF;
    if (!file->f_ops->llseek)
        return -ESPIPE;
    if (!FILE_READ(file))
        return -EBADF;

    kiov_init(&iov, &vec, 1, len, true);
    return vfs_read(&iocb, &iov);
}

long sys_write(struct trap_frame *frame)
{
    unsigned int fd = syscallarg(frame, 0);
    void *buffer = (void *)syscallarg(frame, 1);
    size_t len = MIN(syscallarg(frame, 2), (size_t)SSIZE_MAX);
    struct file *file __free(fput) = fd_open(current->fdtable, fd);
    struct iovec vec = {.iov_base = buffer, .iov_len = len};
    struct kiov iov;

    if (!file || !FILE_WRITE(file))
        return -EBADF;
    kiov_init(&iov, &vec, 1, len, true);
    return file_write(file, &iov);
}

long sys_pwrite64(struct trap_frame *frame)
{
    unsigned int fd = syscallarg(frame, 0);
    void *buffer = (void *)syscallarg(frame, 1);
    size_t len = MIN(syscallarg(frame, 2), (size_t)SSIZE_MAX);
    loff_t offset = syscallarg(frame, 3);

    struct file *file __free(fput) = fd_open(current->fdtable, fd);
    struct kiocb iocb = {.file = file, .pos = offset};
    struct iovec vec = {.iov_base = buffer, .iov_len = len};
    struct kiov iov;

    if (offset < 0)
        return -EINVAL;
    if (!file)
        return -EBADF;
    if (!file->f_ops->llseek)
        return -ESPIPE;
    if (!FILE_WRITE(file))
        return -EBADF;

    kiov_init(&iov, &vec, 1, len, true);
    return vfs_write(&iocb, &iov);
}

static int import_iovec(const struct iovec *uvec, int nr, struct iovec **vec,
                        struct kiov *iov)
{
    struct iovec *buffer __free(kfree) = NULL;
    size_t count = 0;
    size_t bytes;

    if (nr < 0 || nr > UIO_MAXIOV)
        return -EINVAL;
    if (!nr) {
        kiov_init(iov, NULL, 0, 0, true);
        return 0;
    }
    bytes = (size_t)nr * sizeof(*buffer);
    buffer = kmalloc(bytes);
    if (!buffer)
        return -ENOMEM;
    if (copy_from_user(buffer, uvec, bytes))
        return -EFAULT;
    for (int i = 0; i < nr; ++i) {
        if (buffer[i].iov_len > (size_t)SSIZE_MAX - count)
            return -EINVAL;
        count += buffer[i].iov_len;
    }
    kiov_init(iov, buffer, (size_t)nr, count, true);
    *vec = move(buffer);
    return 0;
}

long sys_readv(struct trap_frame *frame)
{
    unsigned int fd = syscallarg(frame, 0);
    const struct iovec *uvec = (const struct iovec *)syscallarg(frame, 1);
    int nr = (int)syscallarg(frame, 2);
    struct file *file __free(fput) = fd_open(current->fdtable, fd);
    struct iovec *vec __free(kfree) = NULL;
    struct kiov iov;
    int ret;

    if (!file || !FILE_READ(file))
        return -EBADF;
    ret = import_iovec(uvec, nr, &vec, &iov);
    if (ret)
        return ret;
    return file_read(file, &iov);
}

long sys_writev(struct trap_frame *frame)
{
    unsigned int fd = syscallarg(frame, 0);
    const struct iovec *uvec = (const struct iovec *)syscallarg(frame, 1);
    int nr = (int)syscallarg(frame, 2);
    struct file *file __free(fput) = fd_open(current->fdtable, fd);
    struct iovec *vec __free(kfree) = NULL;
    struct kiov iov;
    int ret;

    if (!file || !FILE_WRITE(file))
        return -EBADF;
    ret = import_iovec(uvec, nr, &vec, &iov);
    if (ret)
        return ret;
    return file_write(file, &iov);
}

long sys_faccessat(struct trap_frame *frame)
{
    int dirfd = (int)syscallarg(frame, 0);
    const char *upath = (const char *)syscallarg(frame, 1);
    int mode = (int)syscallarg(frame, 2);
    struct path resolved = {0};
    char *path __free(kfree) = NULL;
    int ret;

    if (mode & ~S_IRWXO)
        return -EINVAL;
    path = get_pathname(upath);
    if (IS_ERR(path))
        return PTR_ERR(move(path));

    ret = vfs_lookup(dirfd, path, PATH_FOLLOW, &resolved);
    if (ret)
        return ret;

    path_put(&resolved);
    return 0;
}

long sys_fchmodat(struct trap_frame *frame)
{
    int dirfd = (int)syscallarg(frame, 0);
    const char *upath = (const char *)syscallarg(frame, 1);
    struct path resolved = {0};
    char *path __free(kfree) = NULL;
    int ret;

    path = get_pathname(upath);
    if (IS_ERR(path))
        return PTR_ERR(move(path));

    ret = vfs_lookup(dirfd, path, PATH_FOLLOW, &resolved);
    if (ret)
        return ret;

    path_put(&resolved);
    return 0;
}

long sys_utimensat(struct trap_frame *frame)
{
    int dirfd = (int)syscallarg(frame, 0);
    const char *upath = (const char *)syscallarg(frame, 1);
    int flags = (int)syscallarg(frame, 3);
    struct path resolved = {0};
    char *path __free(kfree) = NULL;
    int ret;

    if (flags & ~AT_SYMLINK_NOFOLLOW)
        return -EINVAL;
    if (!upath && dirfd != AT_FDCWD) {
        struct file *file __free(fput) = NULL;

        if (flags)
            return -EINVAL;
        file = fd_open(current->fdtable, dirfd);
        return file ? 0 : -EBADF;
    }
    path = get_pathname(upath);
    if (IS_ERR(path))
        return PTR_ERR(move(path));
    ret = vfs_lookup(dirfd, path, flags & AT_SYMLINK_NOFOLLOW ? 0 : PATH_FOLLOW,
                     &resolved);
    if (ret)
        return ret;
    path_put(&resolved);
    return 0;
}
