#ifndef _NUVIX_VFS_H
#define _NUVIX_VFS_H

#include <nuvix/refcount.h>
#include <nuvix/mutex.h>
#include <nuvix/ctypes.h>

#define O_RDONLY  00000000
#define O_WRONLY  00000001
#define O_RDWR    00000002
#define O_ACCMODE 00000003

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

void fput(struct file *file);

int vfs_mount(void);

struct file *vfs_open(const char *path, unsigned long flags);

#endif /* ifndef _NUVIX_VFS_H */
