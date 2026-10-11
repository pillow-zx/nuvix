#include <nuvix/vfs.h>

#include <nuvix/errno.h>

void super_init(struct super_block *sb, struct blkdev *bdev,
                const struct super_operations *ops, void *private)
{
    sb->s_bdev = bdev;
    sb->s_ops = ops;
    sb->s_private = private;
    hash_init(sb->s_icache.entries);
    wait_queue_init(&sb->s_icache.wait);
    mutex_init(&sb->s_lock);
    sb->s_root = NULL;
}

int mount_init(struct mount *mnt, struct super_block *sb, uint64_t ino)
{
    struct inode *inode __free(iput) = NULL;
    struct dentry *root __free(dput) = NULL;

    might_sleep();
    Assert(!sb->s_root, "root already installed\n");
    inode = iget(sb, ino);
    if (IS_ERR(inode))
        return PTR_ERR(inode);
    if (!S_ISDIR(inode->i_mode))
        return -ENOTDIR;
    root = dentry_alloc(sb, NULL, "/", 1);
    if (!root)
        return -ENOMEM;
    root->d_inode = move(inode);
    sb->s_root = root;
    mnt->sb = sb;
    mnt->root = root;
    keep(root);
    return 0;
}
