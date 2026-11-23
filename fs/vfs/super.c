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

const struct file_system_type *vfs_find_type(const char *name)
{
    struct file_system_type *type;

    list_for_each_entry (type, &filesystems, node)
        if (!strcmp(type->name, name))
            return type;
    return NULL;
}

const struct file_system_type *vfs_next_type(const struct file_system_type *type)
{
    const struct list_head *next = type ? type->node.next : filesystems.next;

    if (next == &filesystems)
        return NULL;
    return list_entry(next, struct file_system_type, node);
}

static struct super_block *open_super(struct blkdev *dev,
                                      const struct file_system_type *type,
                                      unsigned int flags)
{
    struct super_block *sb __free(kfree) = kzalloc(sizeof(*sb));
    int ret;

    if (!sb)
        return ERR_PTR(-ENOMEM);
    sb->s_bdev = dev;
    sb->s_type = type;
    ret = type->open(sb, flags);
    if (ret)
        return ERR_PTR(ret);
    return move(sb);
}

struct super_block *vfs_open_super(struct blkdev *dev,
                                   const struct file_system_type *type,
                                   unsigned int flags)
{
    struct super_block *sb;
    struct file_system_type *entry;
    int ret;

    if (type && type->nodev)
        return open_super(NULL, type, flags);
    if (!dev)
        return ERR_PTR(-ENODEV);
    if (dev->ops->claim) {
        ret = dev->ops->claim(dev, flags);
        if (ret)
            return ERR_PTR(ret);
    }

    if (type) {
        sb = open_super(dev, type, flags);
        if (sb == ERR_PTR(-ENODATA))
            sb = ERR_PTR(-EINVAL);
    } else {
        sb = ERR_PTR(-ENODEV);
        list_for_each_entry (entry, &filesystems, node) {
            if (entry->nodev)
                continue;
            sb = open_super(dev, entry, flags);
            if (sb != ERR_PTR(-ENODATA))
                break;
            sb = ERR_PTR(-ENODEV);
        }
    }

    if (IS_ERR(sb) && dev->ops->release)
        dev->ops->release(dev);
    return sb;
}

void vfs_close_super(struct super_block *sb, unsigned int flags)
{
    struct blkdev *dev = sb->s_bdev;

    if (dev) {
        mapping_wblock();
        mapping_sync_all();
        if (flags & MNT_WRITE) {
            int ret = blk_flush(dev);

            if (ret)
                pr_warn("block device flush failed (%d)\n", ret);
        }
    }
    dput(sb->s_root);
    sb->s_type->close(sb);
    kfree(sb);
    if (dev) {
        mapping_wbunlock();
        if (dev->ops->release)
            dev->ops->release(dev);
    }
}
