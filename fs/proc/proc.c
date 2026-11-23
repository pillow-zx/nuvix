#include <nuvix/errno.h>
#include <nuvix/layout.h>
#include <nuvix/macro.h>
#include <nuvix/module.h>
#include <nuvix/pid.h>
#include <nuvix/slab.h>
#include <nuvix/string.h>
#include <nuvix/vfs.h>
#include "proc.h"

#define PROC_FIRST_FILE_INO 2

static const struct inode_operations proc_file_iops;

static const struct {
    const char *name;
    umode_t mode;
    const struct inode_operations *iops;
    const struct file_operations *fops;
} proc_files[] = {
    {"meminfo", S_IFREG | 0444, &proc_file_iops, &meminfo_fops},
    {"stat", S_IFREG | 0444, &proc_file_iops, &stat_fops},
    {"buddyinfo", S_IFREG | 0444, &proc_file_iops, &buddyinfo_fops},
    {"filesystems", S_IFREG | 0444, &proc_file_iops, &filesystems_fops},
    {"self", S_IFLNK | 0777, &proc_self_iops, NULL},
};

loff_t proc_llseek(struct file *file, loff_t offset, int whence)
{
    loff_t pos = offset;
    int ret = 0;

    mutex_lock(&file->f_pos_lock);
    switch (whence) {
    case SEEK_SET:
        break;
    case SEEK_CUR:
        if (check_add_overflow(file->f_pos, offset, &pos))
            ret = -EOVERFLOW;
        break;
    default:
        ret = -EINVAL;
        break;
    }
    if (!ret && pos < 0)
        ret = -EINVAL;
    if (!ret)
        file->f_pos = pos;
    mutex_unlock(&file->f_pos_lock);
    return ret ? ret : pos;
}

static struct dentry *proc_lookup(struct inode *dir, struct dentry *dentry)
{
    for (int i = 0; i < ARRLEN(proc_files); ++i) {
        struct inode *inode;

        if (strcmp(dentry->d_name, proc_files[i].name))
            continue;
        inode = i_alloc(dir->i_sb);
        if (!inode)
            return ERR_PTR(-ENOMEM);
        inode->i_ino = PROC_FIRST_FILE_INO + i;
        inode->i_mode = proc_files[i].mode;
        inode->i_nlink = 1;
        inode->i_ops = proc_files[i].iops;
        inode->i_fops = proc_files[i].fops;
        dentry->d_inode = inode;
        return dentry;
    }
    return proc_lookup_pid(dir, dentry);
}

static int proc_readdir(struct file *file, struct dirctx *ctx)
{
    (void)file;
    if (ctx->pos == 0) {
        if (!dir_emit(ctx, ".", 1, 1, PROC_ROOT_INO, DT_DIR))
            return 0;
        ++ctx->pos;
    }
    if (ctx->pos == 1) {
        if (!dir_emit(ctx, "..", 2, 2, PROC_ROOT_INO, DT_DIR))
            return 0;
        ++ctx->pos;
    }
    while ((uint64_t)ctx->pos < 2 + ARRLEN(proc_files)) {
        size_t i = ctx->pos - 2;
        const char *name = proc_files[i].name;
        unsigned int type = S_ISLNK(proc_files[i].mode) ? DT_LNK : DT_REG;

        if (!dir_emit(ctx, name, strlen(name), ctx->pos + 1,
                      PROC_FIRST_FILE_INO + i, type))
            return 0;
        ++ctx->pos;
    }
    proc_emit_pids(ctx, 2 + ARRLEN(proc_files));
    return 0;
}

static const struct file_operations proc_dir_fops = {
    .llseek = proc_llseek,
    .readdir = proc_readdir,
};

int proc_getattr(struct inode *inode, struct kstat *stat)
{
    stat->ino = inode->i_ino;
    stat->mode = inode->i_mode;
    stat->nlink = inode->i_nlink;
    stat->blksize = PAGE_SIZE;
    return 0;
}

static const struct inode_operations proc_dir_iops = {
    .lookup = proc_lookup,
    .getattr = proc_getattr,
};

static const struct inode_operations proc_file_iops = {
    .getattr = proc_getattr,
};

static struct inode *proc_alloc_inode(struct super_block *sb)
{
    struct proc_inode *pi = kzalloc(sizeof(*pi));

    (void)sb;
    if (!pi)
        return NULL;
    return &pi->vfs_inode;
}

static void proc_destroy_inode(struct inode *inode)
{
    struct proc_inode *pi = proc_i(inode);

    pid_put(pi->pid);
    kfree(pi);
}

static const struct super_operations proc_sops = {
    .alloc_inode = proc_alloc_inode,
    .destroy_inode = proc_destroy_inode,
};

static int procfs_open(struct super_block *sb, unsigned int flags)
{
    struct inode *dir __free(iput) = NULL;
    struct dentry *root;

    (void)flags;
    sb->s_ops = &proc_sops;
    sb->s_flags |= SB_RDONLY;
    dir = i_alloc(sb);
    if (!dir)
        return -ENOMEM;
    root = d_alloc(NULL, "/", 1);
    if (IS_ERR(root))
        return PTR_ERR(root);

    dir->i_ino = PROC_ROOT_INO;
    dir->i_mode = S_IFDIR | 0555;
    dir->i_nlink = 2;
    dir->i_ops = &proc_dir_iops;
    dir->i_fops = &proc_dir_fops;
    root->d_inode = move(dir);
    sb->s_root = root;
    return 0;
}

static void procfs_close(struct super_block *sb)
{
    (void)sb;
}

static struct file_system_type proc_type = {
    .name = "proc",
    .nodev = true,
    .open = procfs_open,
    .close = procfs_close,
};

MODULE_FILESYSTEM_INIT(proc, 80);
