#ifndef _NUVIX_FS_PROC_H
#define _NUVIX_FS_PROC_H

#include <nuvix/printk.h>
#include <nuvix/fs.h>

#define PROC_U64_DIGITS 20
#define PROC_ROOT_INO   1

struct pid;

struct proc_inode {
    struct inode vfs_inode;
    struct pid *pid;
};

static inline __printf(2, 3) int proc_format(char *buf, const char *fmt, ...)
{
    va_list args;
    int len;

    va_start(args, fmt);
    len = vsprintf(buf, fmt, args);
    va_end(args);
    return len;
}

static inline struct proc_inode *proc_i(struct inode *inode)
{
    return container_of(inode, struct proc_inode, vfs_inode);
}

extern const struct file_operations meminfo_fops;
extern const struct file_operations stat_fops;
extern const struct file_operations buddyinfo_fops;
extern const struct file_operations filesystems_fops;
extern const struct inode_operations proc_self_iops;

int proc_getattr(struct inode *inode, struct kstat *stat);
struct dentry *proc_lookup_pid(struct inode *dir, struct dentry *dentry);
void proc_emit_pids(struct dirctx *ctx, loff_t base);

loff_t proc_llseek(struct file *file, loff_t offset, int whence);

int proc_open(struct file *file, size_t size,
                   int (*show)(struct inode *, char *));
ssize_t proc_read(struct kiocb *iocb, struct kiov *iov);
int proc_release(struct file *file);

#endif /* _NUVIX_FS_PROC_H */
