#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/macro.h>
#include <nuvix/path.h>
#include <nuvix/string.h>
#include <nuvix/task.h>
#include <nuvix/vfs.h>

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

static int path_lookup_locked(const struct path *base, const char *name,
                              struct path *result)
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

int path_lookup(const struct path *base, const char *name, struct path *result)
{
    struct inode *dir = base->dentry->d_inode;
    int ret;

    if (!dir)
        return -ENOENT;
    if (!S_ISDIR(dir->i_mode))
        return -ENOTDIR;

    mutex_lock(&dir->mutex);
    ret = path_lookup_locked(base, name, result);
    mutex_unlock(&dir->mutex);
    return ret;
}

static int path_walk(const char *path, bool parent, struct path *result,
                     const char **last, size_t *last_len)
{
    struct path base = {0};
    struct path cur;
    const char *cursor = path;
    const char *start;
    size_t len = 0;
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
    start = cursor;

    while (*cursor) {
        char name[VFS_NAME_MAX + 1];
        struct path next;

        start = cursor;
        while (*cursor && *cursor != '/')
            ++cursor;
        len = (size_t)(cursor - start);
        if (len > (size_t)VFS_NAME_MAX) {
            ret = -ENAMETOOLONG;
            goto out;
        }
        if (parent && (!*cursor || !cursor[1]))
            break;
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
    if ((parent || path[strlen(path) - 1] == '/') &&
        !S_ISDIR(cur.dentry->d_inode->i_mode)) {
        ret = -ENOTDIR;
        goto out;
    }

    if (parent) {
        *last = start;
        *last_len = len;
    }
    *result = move(cur);
    return 0;

out:
    path_put(&cur);
    return ret;
}

int path_resolve(const char *path, struct path *result)
{
    return path_walk(path, false, result, NULL, NULL);
}

int path_resolve_parent(const char *path, struct path *parent,
                        const char **name, size_t *len)
{
    return path_walk(path, true, parent, name, len);
}
