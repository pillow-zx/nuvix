#ifndef _NUVIX_VFS_H
#define _NUVIX_VFS_H

#include <nuvix/refcount.h>
#include <nuvix/mutex.h>
#include <nuvix/ctypes.h>
#include <nuvix/cleanup.h>

#define PATH_MAX         4096
#define AT_FDCWD         (-100)

#define O_RDONLY         00000000
#define O_WRONLY         00000001
#define O_RDWR           00000002
#define O_ACCMODE        00000003
#define O_LARGEFILE      00100000

#define FILE_READ(file)  (((file)->f_flags & O_ACCMODE) != O_WRONLY)
#define FILE_WRITE(file) (((file)->f_flags & O_ACCMODE) != O_RDONLY)
#define FILE_RDWR(file)  (((file)->f_flags & O_ACCMODE) == O_RDWR)

struct file;
struct file_operations;

typedef long long loff_t;

struct file_operations {
    ssize_t (*read)(struct file *, char *, size_t, loff_t *);
    ssize_t (*write)(struct file *, const char *, size_t, loff_t *);
    int (*fsync)(struct file *);
    int (*release)(struct file *);
};

struct file {
    refcount_t f_count;
    const struct file_operations *f_ops;
    unsigned int f_flags;
    struct mutex f_pos_lock;
    int64_t f_pos;
    void *f_private;
};

struct file *fget(struct file *file);

int fput(struct file *file);

DECLARE_FREE(fput, struct file *, if (_T) fput(_T));

struct iovec {
    void *iov_base;
    size_t iov_len;
};

#define UIO_MAXIOV 64

int vfs_mount(void);

struct file *vfs_open(const char *path, unsigned long flags);

ssize_t vfs_read(struct file *, void *, size_t, loff_t);

ssize_t vfs_write(struct file *, const void *, size_t, loff_t);

#endif /* ifndef _NUVIX_VFS_H */
