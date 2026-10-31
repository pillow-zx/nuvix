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
#include <nuvix/uaccess.h>
#include <asm/trap.h>

struct file *vfs_open(const char *path, unsigned long flags)
{
    struct path resolved = {0};
    struct file *file __free(kfree) = NULL;
    unsigned int access = flags & O_ACCMODE;
    struct inode *inode;
    int ret;

    if (!*path)
        return ERR_PTR(-ENOENT);
    if (flags & ~(O_ACCMODE | O_NONBLOCK | O_LARGEFILE | O_DIRECTORY |
                  O_NOFOLLOW | O_CLOEXEC))
        return ERR_PTR(-EOPNOTSUPP);
    if (access == O_ACCMODE)
        return ERR_PTR(-EINVAL);

    ret = path_resolve(path, &resolved);
    if (ret)
        return ERR_PTR(ret);

    inode = resolved.dentry->d_inode;
    if (!inode) {
        ret = -ENOENT;
        goto fail;
    }
    if (!inode->i_fops) {
        ret = -EOPNOTSUPP;
        goto fail;
    }
    if ((flags & O_DIRECTORY) && !S_ISDIR(inode->i_mode)) {
        ret = -ENOTDIR;
        goto fail;
    }
    if (S_ISDIR(inode->i_mode) && access != O_RDONLY) {
        ret = -EISDIR;
        goto fail;
    }

    file = kzalloc(sizeof(*file));
    if (!file) {
        ret = -ENOMEM;
        goto fail;
    }

    refcount_set(&file->f_count, 1);
    mutex_init(&file->f_pos_lock);
    file->f_flags = flags;
    file->f_pos = 0;
    file->f_ops = inode->i_fops;
    file->f_path = move(resolved);

    if (file->f_ops->open) {
        ret = file->f_ops->open(file);
        if (ret) {
            path_put(&file->f_path);
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
    if (!iov->count)
        return 0;
    return iocb->file->f_ops->read(iocb, iov);
}

ssize_t vfs_write(struct kiocb *iocb, struct kiov *iov)
{
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
    char *path __free(kfree) = NULL;
    struct file *file __free(fput) = NULL;
    long len;
    int fd;

    path = kmalloc(PATH_MAX);
    if (!path)
        return -ENOMEM;
    len = strncpy_from_user(path, upath, PATH_MAX);
    if (len < 0)
        return len;
    if (len == PATH_MAX)
        return -ENAMETOOLONG;
    if (path[0] != '/' && dirfd != AT_FDCWD)
        return -EOPNOTSUPP;

    file = vfs_open(path, flags);
    if (IS_ERR(file))
        return PTR_ERR(move(file));
    fd = fd_alloc(current->fdtable);
    if (fd < 0)
        return fd;
    fd_install(current->fdtable, (unsigned long)fd, move(file));
    return fd;
}

long sys_close(struct trap_frame *frame)
{
    unsigned int fd = syscallarg(frame, 0);

    return fd_close(current->fdtable, fd);
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

long sys_fchmodat(struct trap_frame *frame)
{
    (void)frame;
    return 0;
}
