#ifndef _NUVIX_FS_H
#define _NUVIX_FS_H

#include <nuvix/ctypes.h>
#include <nuvix/mutex.h>
#include <nuvix/refcount.h>
#include <nuvix/stat.h>
#include <nuvix/kiov.h>
#include <nuvix/path.h>
#include <nuvix/cleanup.h>
#include <nuvix/hlist.h>

#define VFS_NAME_MAX     255

#define O_RDONLY         00000000
#define O_WRONLY         00000001
#define O_RDWR           00000002
#define O_ACCMODE        00000003
#define O_NONBLOCK       00004000
#define O_LARGEFILE      00100000
#define O_DIRECTORY      00200000
#define O_NOFOLLOW       00400000
#define O_CLOEXEC        02000000

#define SEEK_SET         0
#define SEEK_CUR         1
#define SEEK_END         2

#define F_DUPFD          0
#define F_GETFD          1
#define F_SETFD          2
#define F_GETFL          3
#define F_SETFL          4
#define F_DUPFD_CLOEXEC  1030

#define FILE_READ(file)  (((file)->f_flags & O_ACCMODE) != O_WRONLY)
#define FILE_WRITE(file) (((file)->f_flags & O_ACCMODE) != O_RDONLY)
#define FILE_RDWR(file)  (((file)->f_flags & O_ACCMODE) == O_RDWR)

struct blkdev;
struct file;
struct inode;
struct dentry;
struct dirctx;
struct super_block;
struct file_operations;
struct inode_operations;

struct kiocb {
    struct file *file;
    loff_t pos;
};

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12
#define DT_WHT     14

typedef bool (*filldir_t)(struct dirctx *, const char *, int, loff_t, u64,
                          unsigned);

struct dirctx {
    filldir_t actor;
    loff_t pos;
};

static __always_inline bool dir_emit(struct dirctx *ctx, const char *name,
                                     int namelen, loff_t offset, u64 ino,
                                     unsigned type)
{
    return ctx->actor(ctx, name, namelen, offset, ino, type);
}

/* equal to linux_dirent64 */
struct dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
};

static_assert(offsetof(struct dirent64, d_name) == 19,
              "invalid dirent64 layout");

struct file_operations {
    int (*open)(struct file *);
    loff_t (*llseek)(struct file *, loff_t, int);
    ssize_t (*read)(struct kiocb *, struct kiov *);
    ssize_t (*write)(struct kiocb *, struct kiov *);
    long (*ioctl)(struct file *, unsigned int, unsigned long);
    int (*readdir)(struct file *, struct dirctx *);
    int (*fsync)(struct file *);
    int (*release)(struct file *);
};

struct file {
    refcount_t f_count;
    const struct file_operations *f_ops;
    unsigned int f_flags;
    struct mutex f_pos_lock;
    int64_t f_pos;
    struct inode *f_inode;
    struct path f_path;
    void *f_private;
};

struct inode_operations {
    struct dentry *(*lookup)(struct inode *, struct dentry *);
    int (*getattr)(struct inode *, struct kstat *);
    int (*create)(struct inode *, struct dentry *);
    int (*link)(struct dentry *, struct inode *, struct dentry *);
    int (*unlink)(struct inode *, struct dentry *);
    int (*symlink)(struct inode *, struct dentry *, const char *);
    const char *(*readlink)(struct inode *, void (*fn)(void *), void *arg);
    struct dentry *(*mkdir)(struct inode *, struct dentry *);
    int (*rmdir)(struct inode *, struct dentry *);
    /* Frees the complete allocation containing this inode. */
    void (*destroy)(struct inode *);
};

struct super_block {
    struct blkdev *s_bdev;
    struct dentry *s_root;
    void *s_fs_info;
};

struct inode {
    refcount_t i_count;
    struct super_block *i_sb;
    uint64_t i_ino;
    loff_t i_size;
    uint64_t i_blocks;
    struct timespec64 i_atime;
    struct timespec64 i_mtime;
    struct timespec64 i_ctime;
    unsigned int i_nlink;
    unsigned int i_mode;
    const struct inode_operations *i_ops;
    const struct file_operations *i_fops;
    struct hlist_node i_hash;
    void *i_private;
};

struct dentry {
    refcount_t d_count;
    struct dentry *d_parent;
    struct inode *d_inode;
    char *d_name;
    unsigned int d_nlen;
};

struct file *fget(struct file *file);

int fput(struct file *file);

DECLARE_FREE(fput, struct file *, if (_T) fput(_T));

struct inode *i_alloc(void);

void ihold(struct inode *inode);

void iput(struct inode *inode);

struct inode *icache_find(struct super_block *sb, uint64_t ino);

struct inode *icache_insert(struct inode *inode);

struct dentry *d_alloc(struct dentry *parent, const char *name, size_t len);

void dget(struct dentry *dentry);

void dput(struct dentry *dentry);

#endif /* ifndef _NUVIX_FS_H */
