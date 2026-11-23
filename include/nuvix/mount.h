#ifndef _NUVIX_MOUNT_H
#define _NUVIX_MOUNT_H

#include <nuvix/list.h>
#include <nuvix/mutex.h>
#include <nuvix/refcount.h>

struct dentry;
struct path;
struct super_block;

#define MNT_READ  (1U << 0)
#define MNT_WRITE (1U << 1)

struct mount {
    struct super_block *sb;
    struct mount *parent;
    struct dentry *mntpoint;
    refcount_t refs;
    struct list_head node;
    unsigned int flags;
    bool unmounting;
};

extern struct mutex mount_mutex;

int vfs_mount(void);
int vfs_get_root(struct path *path);
void mnt_put(struct mount *mnt);
void vfs_follow_mount(struct path *path);
bool is_mntpoint(const struct path *path);

#endif
