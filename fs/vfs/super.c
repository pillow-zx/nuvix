#include <nuvix/blkdev.h>
#include <nuvix/errno.h>
#include <nuvix/mapping.h>
#include <nuvix/printk.h>
#include <nuvix/slab.h>
#include <nuvix/string.h>
#include <nuvix/vfs.h>

static LIST_HEAD(filesystems);

void vfs_register(struct file_system_type *type)
{
    list_add_tail(&type->node, &filesystems);
}

struct super_block *vfs_open_bdev(struct blkdev *dev, const char *name,
                                  unsigned int flags)
{
    struct file_system_type *type;

    if (!dev)
        return ERR_PTR(-ENODEV);

    list_for_each_entry (type, &filesystems, node) {
        struct super_block *sb __free(kfree) = NULL;
        int ret;

        if (name && strcmp(type->name, name))
            continue;
        sb = kzalloc(sizeof(*sb));
        if (!sb)
            return ERR_PTR(-ENOMEM);
        sb->s_bdev = dev;
        sb->s_type = type;
        ret = type->open(sb, flags);
        if (!ret)
            return move(sb);
        if (ret != -ENODATA)
            return ERR_PTR(ret);
        if (name)
            return ERR_PTR(-EINVAL);
    }

    return ERR_PTR(-ENODEV);
}

void vfs_close_bdev(struct super_block *sb, unsigned int flags)
{
    mapping_wblock();
    mapping_sync_all();
    if (flags & MNT_WRITE) {
        int ret = blk_flush(sb->s_bdev);

        if (ret)
            pr_warn("block device flush failed (%d)\n", ret);
    }
    dput(sb->s_root);
    sb->s_type->close(sb);
    kfree(sb);
    mapping_wbunlock();
}
