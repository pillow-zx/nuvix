#ifndef _NUVIX_PATH_H
#define _NUVIX_PATH_H

#include <nuvix/cleanup.h>

struct dentry;

struct path {
    struct dentry *dentry;
};

void path_get(const struct path *path);

void path_put(struct path *path);

int path_lookup(const struct path *base, const char *name, struct path *result);

int path_resolve(const char *path, struct path *result);

#endif /* ifndef _NUVIX_PATH_H */
