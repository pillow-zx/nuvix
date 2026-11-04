#ifndef _NUVIX_VFS_H
#define _NUVIX_VFS_H

#include <nuvix/fs.h>
#include <nuvix/cleanup.h>

#define PATH_MAX            4096
#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100

#define UIO_MAXIOV          64

int vfs_mount(void);

int vfs_get_root(struct path *path);

struct file *vfs_open(const char *path, unsigned long flags);

ssize_t vfs_read(struct kiocb *, struct kiov *);

ssize_t vfs_write(struct kiocb *, struct kiov *);

#ifdef CONFIG_EXT2_FS
int ext2_mount(struct super_block *sb);
#endif

#endif /* ifndef _NUVIX_VFS_H */
