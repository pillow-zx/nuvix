#ifndef _NUVIX_VFS_H
#define _NUVIX_VFS_H

#include <nuvix/fs.h>
#include <nuvix/cleanup.h>

#define PATH_MAX            4096
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_SYMLINK_FOLLOW   0x400
#define AT_EMPTY_PATH       0x1000

#define UIO_MAXIOV          64

int vfs_mount(void);

int vfs_get_root(struct path *path);

char *get_pathname(const char *upath);

struct file *vfs_open(int dirfd, const char *path, unsigned long flags);

ssize_t vfs_read(struct kiocb *, struct kiov *);

ssize_t vfs_write(struct kiocb *, struct kiov *);

#ifdef CONFIG_EXT2_FS
int ext2_mount(struct super_block *sb);
#endif

#endif /* ifndef _NUVIX_VFS_H */
