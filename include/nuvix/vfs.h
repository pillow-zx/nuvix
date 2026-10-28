#ifndef _NUVIX_VFS_H
#define _NUVIX_VFS_H

#include <nuvix/fs.h>
#include <nuvix/cleanup.h>

#define PATH_MAX   4096
#define AT_FDCWD   (-100)

#define UIO_MAXIOV 64

int vfs_mount(void);

struct file *vfs_open(const char *path, unsigned long flags);

ssize_t vfs_read(struct kiocb *, struct kiov *);

ssize_t vfs_write(struct kiocb *, struct kiov *);

#endif /* ifndef _NUVIX_VFS_H */
