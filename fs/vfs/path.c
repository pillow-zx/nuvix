#include <nuvix/errno.h>
#include <nuvix/fdtable.h>
#include <nuvix/fs.h>
#include <nuvix/macro.h>
#include <nuvix/path.h>
#include <nuvix/slab.h>
#include <nuvix/string.h>
#include <nuvix/task.h>
#include <nuvix/vfs.h>

#define MAX_SYMLINKS 40

void path_get(const struct path *path)
{
    dget(path->dentry);
}

void path_put(struct path *path)
{
    if (path->dentry)
        dput(path->dentry);
    path->dentry = NULL;
}

int vfs_lookone(const struct path *base, const char *name, struct path *result)
{
    struct dentry *dentry;
    struct dentry *found;
    struct inode *dir = base->dentry->d_inode;
    size_t len;

    Assert(dir && S_ISDIR(dir->i_mode),
           "locked lookup requires a positive directory\n");
    assert_mutex_locked(&dir->mutex);

    len = strnlen(name, (size_t)VFS_NAME_MAX + 1);
    if (!len)
        return -ENOENT;
    if (len > (size_t)VFS_NAME_MAX)
        return -ENAMETOOLONG;
    if (strchr(name, '/'))
        return -EINVAL;

    if (len == 1 && name[0] == '.') {
        path_get(base);
        result->dentry = base->dentry;
        return 0;
    }

    if (len == 2 && name[0] == '.' && name[1] == '.') {
        dentry = base->dentry->d_parent;
        if (!dentry)
            dentry = base->dentry;
        dget(dentry);
        result->dentry = dentry;
        return 0;
    }

    if (!dir->i_nlink)
        return -ENOENT;
    if (!dir->i_ops || !dir->i_ops->lookup)
        return -EOPNOTSUPP;

    found = dcache_find(base->dentry, name, len);
    if (found)
        goto out;

    dentry = d_alloc(base->dentry, name, len);
    if (IS_ERR(dentry)) {
        found = dentry;
        goto out;
    }

    found = dir->i_ops->lookup(dir, dentry);
    if (IS_ERR(found)) {
        dput(dentry);
        goto out;
    }
    found = dcache_insert(found);

out:
    if (IS_ERR(found))
        return PTR_ERR(found);
    result->dentry = found;
    return 0;
}

static int path_start(int dirfd, const char *name, struct path *result)
{
    struct file *file __free(fput) = NULL;

    if (*name == '/')
        return vfs_get_root(result);
    if (dirfd == AT_FDCWD) {
        if (!current->cwd.dentry)
            return -ENOENT;
        *result = current->cwd;
    } else {
        file = fd_open(current->fdtable, dirfd);
        if (!file)
            return -EBADF;
        if (!file->f_path.dentry)
            return *name ? -ENOTDIR : -ENOENT;
        *result = file->f_path;
    }
    if (*name && !S_ISDIR(result->dentry->d_inode->i_mode)) {
        result->dentry = NULL;
        return -ENOTDIR;
    }
    path_get(result);
    return 0;
}

int vfs_lookup(int dirfd, const char *path, unsigned int flags,
               struct path *result)
{
    struct path cur = {0};
    struct path next = {0};
    char *pending __free(kfree) = NULL;
    char *link __free(kfree) = NULL;
    const char *cursor;
    unsigned int links = 0;
    size_t len = strnlen(path, PATH_MAX);
    int ret;

    if (!len && !(flags & PATH_EMPTY))
        return -ENOENT;
    if (len == PATH_MAX)
        return -ENAMETOOLONG;
    ret = path_start(dirfd, path, &cur);
    if (ret)
        return ret;
    pending = kmalloc(PATH_MAX);
    if (!pending) {
        ret = -ENOMEM;
        goto out;
    }
    memcpy(pending, path, len + 1);
    cursor = pending;

    for (;;) {
        struct inode *dir = cur.dentry->d_inode;
        char name[VFS_NAME_MAX + 1];
        const char *tail;
        bool slash;

        while (*cursor == '/')
            cursor++;
        if (!*cursor)
            break;
        tail = cursor;
        while (*tail && *tail != '/')
            tail++;
        len = tail - cursor;
        if (len > VFS_NAME_MAX) {
            ret = -ENAMETOOLONG;
            goto out;
        }
        memcpy(name, cursor, len);
        name[len] = '\0';
        slash = *tail == '/';
        mutex_lock(&dir->mutex);
        ret = vfs_lookone(&cur, name, &next);
        mutex_unlock(&dir->mutex);
        if (ret)
            goto out;
        if (!next.dentry->d_inode) {
            ret = -ENOENT;
            goto out;
        }
        if (S_ISLNK(next.dentry->d_inode->i_mode) &&
            (slash || (flags & PATH_FOLLOW))) {
            struct inode *inode = next.dentry->d_inode;
            ssize_t nr;
            size_t rest = strlen(tail);

            if (++links > MAX_SYMLINKS) {
                ret = -ELOOP;
                goto out;
            }
            if (!inode->i_ops->readlink) {
                ret = -EOPNOTSUPP;
                goto out;
            }
            if (!link) {
                link = kmalloc(PATH_MAX);
                if (!link) {
                    ret = -ENOMEM;
                    goto out;
                }
            }
            nr = inode->i_ops->readlink(inode, link, PATH_MAX - 1);
            if (nr <= 0) {
                ret = nr ? nr : -ENOENT;
                goto out;
            }
            if ((size_t)nr + rest >= PATH_MAX) {
                ret = -ENAMETOOLONG;
                goto out;
            }
            memmove(pending + nr, tail, rest + 1);
            memcpy(pending, link, nr);
            path_put(&next);
            if (*pending == '/') {
                path_put(&cur);
                ret = vfs_get_root(&cur);
                if (ret)
                    goto out;
            }
            cursor = pending;
            continue;
        }
        path_put(&cur);
        cur = move(next);
        if (slash && !S_ISDIR(cur.dentry->d_inode->i_mode)) {
            ret = -ENOTDIR;
            goto out;
        }
        cursor = tail;
    }
    *result = move(cur);
    ret = 0;
out:
    path_put(&next);
    path_put(&cur);
    return ret;
}

int vfs_lookparent(int dirfd, const char *path, struct path *parent,
                   const char **name, size_t *len)
{
    char *prefix __free(kfree) = NULL;
    const char *end;
    const char *start;
    size_t size = strnlen(path, PATH_MAX);

    if (!size)
        return -ENOENT;
    if (size == PATH_MAX)
        return -ENAMETOOLONG;
    end = path + size;
    while (end > path && end[-1] == '/')
        end--;
    start = end;
    while (start > path && start[-1] != '/')
        start--;
    *name = start;
    *len = end - start;
    if (*len > VFS_NAME_MAX)
        return -ENAMETOOLONG;
    if (start == path)
        return path_start(dirfd, path, parent);

    size = start - path;
    prefix = kmalloc(size + 1);
    if (!prefix)
        return -ENOMEM;
    memcpy(prefix, path, size);
    prefix[size] = '\0';
    return vfs_lookup(dirfd, prefix, PATH_FOLLOW, parent);
}
