#ifndef _NUVIX_VFS_H
#define _NUVIX_VFS_H

#include <nuvix/fs.h>
#include <nuvix/mount.h>
#include <nuvix/cleanup.h>

#define PATH_MAX            4096
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_SYMLINK_FOLLOW   0x400
#define AT_EMPTY_PATH       0x1000

#define UIO_MAXIOV          64

struct file_system_type {
    const char *name;
    bool nodev;
    int (*open)(struct super_block *sb, unsigned int flags);
    void (*close)(struct super_block *sb);
    struct list_head node;
};

// clang-format off
void vfs_register(struct file_system_type *type);

const struct file_system_type *vfs_find_type(const char *name);
const struct file_system_type *vfs_next_type(const struct file_system_type *type);

struct super_block *vfs_open_super(struct blkdev *dev, const struct file_system_type *type, unsigned int flags);
void vfs_close_super(struct super_block *sb, unsigned int flags);

char *get_pathname(const char *upath);

struct file *vfs_open(int dirfd, const char *path, unsigned long flags);

ssize_t vfs_read(struct kiocb *, struct kiov *);

ssize_t vfs_write(struct kiocb *, struct kiov *);
//clang-format on

#endif /* ifndef _NUVIX_VFS_H */
