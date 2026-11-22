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
    int ret;

    if (!dev)
        return ERR_PTR(-ENODEV);
    if (dev->ops->claim) {
        ret = dev->ops->claim(dev, flags);
        if (ret)
            return ERR_PTR(ret);
    }

    ret = -ENODEV;
    list_for_each_entry (type, &filesystems, node) {
        struct super_block *sb __free(kfree) = NULL;

        if (name && strcmp(type->name, name))
            continue;
        sb = kzalloc(sizeof(*sb));
        if (!sb) {
            ret = -ENOMEM;
            break;
        }
        sb->s_bdev = dev;
        sb->s_type = type;
        ret = type->open(sb, flags);
        if (!ret)
            return move(sb);
        if (ret != -ENODATA)
            break;
        if (name) {
            ret = -EINVAL;
            break;
        }
        ret = -ENODEV;
    }

    if (dev->ops->release)
        dev->ops->release(dev);
    return ERR_PTR(ret);
}

void vfs_close_bdev(struct super_block *sb, unsigned int flags)
{
    struct blkdev *dev = sb->s_bdev;

    mapping_wblock();
    mapping_sync_all();
    if (flags & MNT_WRITE) {
        int ret = blk_flush(dev);

        if (ret)
            pr_warn("block device flush failed (%d)\n", ret);
    }
    dput(sb->s_root);
    sb->s_type->close(sb);
    kfree(sb);
    mapping_wbunlock();
    if (dev->ops->release)
        dev->ops->release(dev);
}
