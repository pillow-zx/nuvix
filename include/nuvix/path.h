#ifndef _NUVIX_PATH_H
#define _NUVIX_PATH_H

struct dentry;

struct path {
    struct dentry *dentry;
};

struct path *path_get(struct path *path);

int path_put(struct path *path);

int path_lookup(const struct path *base, const char *name, struct path *result);

#endif /* ifndef _NUVIX_PATH_H */
