#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/macro.h>
#include <nuvix/path.h>
#include <nuvix/string.h>
#include <nuvix/task.h>
#include <nuvix/vfs.h>

void path_get(const struct path *path)
{
    refcount_inc(&path->dentry->d_count);
}

void path_put(struct path *path)
{
    if (path->dentry)
        dput(path->dentry);
    path->dentry = NULL;
}

int path_lookup(const struct path *base, const char *name, struct path *result)
{
    struct dentry *dentry;
    struct dentry *found;
    struct inode *dir;
    size_t len;
    int ret;

    len = strnlen(name, (size_t)VFS_NAME_MAX + 1);
    if (!len)
        return -ENOENT;
    if (len > (size_t)VFS_NAME_MAX)
        return -ENAMETOOLONG;
    if (strchr(name, '/'))
        return -EINVAL;

    dir = base->dentry->d_inode;
    if (!dir)
        return -ENOENT;
    if (!S_ISDIR(dir->i_mode))
        return -ENOTDIR;

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

    if (!dir->i_ops || !dir->i_ops->lookup)
        return -EOPNOTSUPP;

    dentry = d_alloc(base->dentry, name, len);
    if (IS_ERR(dentry))
        return PTR_ERR(dentry);

    found = dir->i_ops->lookup(dir, dentry);
    if (IS_ERR(found)) {
        ret = PTR_ERR(found);
        dput(dentry);
        return ret;
    }
    if (!found || !found->d_inode) {
        dput(dentry);
        return -ENOENT;
    }

    result->dentry = found;
    return 0;
}

int path_resolve(const char *path, struct path *result)
{
    struct path base = {0};
    struct path cur;
    const char *cursor = path;
    int ret;

    if (!*path)
        return -ENOENT;

    if (*path == '/') {
        if (path[1] == '/')
            return -EINVAL;
        ret = vfs_get_root(&base);
        if (ret)
            return ret;
        ++cursor;
    } else {
        if (!current->cwd.dentry)
            return -ENOENT;
        base = current->cwd;
        path_get(&base);
    }

    cur = move(base);

    while (*cursor) {
        char name[VFS_NAME_MAX + 1];
        const char *start;
        struct path next;
        size_t len;

        start = cursor;
        while (*cursor && *cursor != '/')
            ++cursor;
        len = (size_t)(cursor - start);
        if (len > (size_t)VFS_NAME_MAX) {
            ret = -ENAMETOOLONG;
            goto out;
        }
        memcpy(name, start, len);
        name[len] = '\0';

        ret = path_lookup(&cur, name, &next);
        if (ret)
            goto out;
        path_put(&cur);
        cur = next;

        if (!cur.dentry->d_inode) {
            ret = -ENOENT;
            goto out;
        }
        if (S_ISLNK(cur.dentry->d_inode->i_mode)) {
            ret = -EOPNOTSUPP;
            goto out;
        }

        if (!*cursor)
            break;
        ++cursor;
        if (*cursor == '/') {
            ret = -EINVAL;
            goto out;
        }
    }

    if (!cur.dentry->d_inode) {
        ret = -ENOENT;
        goto out;
    }
    if (path[strlen(path) - 1] == '/' &&
        !S_ISDIR(cur.dentry->d_inode->i_mode)) {
        ret = -ENOTDIR;
        goto out;
    }

    *result = move(cur);
    return 0;

out:
    path_put(&cur);
    return ret;
}
