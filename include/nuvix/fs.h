#ifndef _NUVIX_FS_H
#define _NUVIX_FS_H

#include <nuvix/ctypes.h>
#include <nuvix/mutex.h>
#include <nuvix/refcount.h>
#include <nuvix/stat.h>
#include <nuvix/kiov.h>
#include <nuvix/cleanup.h>

#define O_RDONLY         00000000
#define O_WRONLY         00000001
#define O_RDWR           00000002
#define O_ACCMODE        00000003
#define O_LARGEFILE      00100000

#define FILE_READ(file)  (((file)->f_flags & O_ACCMODE) != O_WRONLY)
#define FILE_WRITE(file) (((file)->f_flags & O_ACCMODE) != O_RDONLY)
#define FILE_RDWR(file)  (((file)->f_flags & O_ACCMODE) == O_RDWR)

struct file;
struct inode;
struct file_operations;
struct inode_operations;

struct kiocb {
    struct file *file;
    loff_t pos;
};

struct file_operations {
    int (*open)(struct file *);
    ssize_t (*read)(struct kiocb *, struct kiov *);
    ssize_t (*write)(struct kiocb *, struct kiov *);
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

struct inode_operations {
    struct inode *(*lookup)(struct inode *, const char *);
    int (*getattr)(struct inode *, struct kstat *);
    void (*destroy)(struct inode *);
};

struct inode {
    refcount_t i_count;
    unsigned int i_mode;
    const struct inode_operations *i_ops;
    const struct file_operations *i_fops;
    void *i_private;
};

struct dentry {
    refcount_t d_count;
    struct dentry *d_parent;
    struct inode *d_inode;
    char *d_name;
};

struct file *fget(struct file *file);

int fput(struct file *file);

DECLARE_FREE(fput, struct file *, if (_T) fput(_T));

#endif /* ifndef _NUVIX_FS_H */
