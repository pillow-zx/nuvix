#ifndef _NUVIX_PATH_H
#define _NUVIX_PATH_H

#include <nuvix/cleanup.h>
#include <nuvix/refcount.h>
#include <nuvix/spinlock.h>

struct dentry;
struct mount;

struct path {
    struct mount *mnt;
    struct dentry *dentry;
};

struct fs_struct {
    refcount_t refs;
    spinlock_t lock;
    struct path cwd;
};

struct fs_struct *fs_alloc(void);
struct fs_struct *fs_clone(struct fs_struct *src);
void fs_get(struct fs_struct *fs);
void fs_put(struct fs_struct *fs);

struct path fs_get_cwd(struct fs_struct *fs);
void fs_set_cwd(struct fs_struct *fs, struct path cwd);

#define AT_FDCWD    (-100)
#define PATH_FOLLOW (1U << 0)
#define PATH_EMPTY  (1U << 1)
#define PATH_CREATE (1U << 2)
#define PATH_EXCL   (1U << 3)

void path_get(const struct path *path);

void path_put(struct path *path);

int vfs_lookup(int fd, const char *path, unsigned int flags, struct path *out);

int vfs_lookparent(int fd, const char *path, struct path *dir,
                   const char **name, size_t *len);

int vfs_lookone(const struct path *dir, const char *name, size_t len,
                struct path *out);

#define vfs_look(path, out) vfs_lookup(AT_FDCWD, (path), PATH_FOLLOW, (out))

#endif /* ifndef _NUVIX_PATH_H */
