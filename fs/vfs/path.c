#include <nuvix/refcount.h>
#include <nuvix/syscall.h>
#include <nuvix/path.h>
#include <nuvix/fs.h>

struct path *path_get(struct path *path)
{
    if (path)
        refcount_inc(&path->dentry->d_count);
    return path;
}

int path_put(struct path *path)
{
    (void)path;
    return 0;
}

int path_lookup(const struct path *base, const char *name, struct path *result)
{
    (void)base;
    (void)name;
    (void)result;
    return 0;
}

