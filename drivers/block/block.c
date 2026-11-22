#include <nuvix/assert.h>
#include <nuvix/blkdev.h>
#include <nuvix/errno.h>
#include <nuvix/fs.h>

static LIST_HEAD(blkdevs);

struct blkdev *blkdev_first(void)
{
    if (list_empty(&blkdevs))
        return NULL;
    return list_first_entry(&blkdevs, struct blkdev, node);
}

struct blkdev *blkdev_find(dev_t dev)
{
    struct blkdev *entry;

    list_for_each_entry (entry, &blkdevs, node)
        if (entry->dev == dev)
            return entry;
    return NULL;
}

void blkdev_register(struct blkdev *dev)
{
    Assert(!blkdev_find(dev->dev), "duplicate block device number\n");
    list_add_tail(&dev->node, &blkdevs);
}

static int blkdev_file_open(struct file *file)
{
    struct blkdev *dev = blkdev_find(file->f_inode->i_rdev);

    if (!dev)
        return -ENXIO;
    if (!dev->fops)
        return -EOPNOTSUPP;
    file->f_ops = dev->fops;
    file->f_private = dev;
    return file->f_ops->open ? file->f_ops->open(file) : 0;
}

const struct file_operations blkdev_file_operations = {
    .open = blkdev_file_open,
};

static int blk_validate(const struct blkdev *dev, uint64_t sector,
                        const struct page *page, size_t offset, size_t length)
{
    if (!dev)
        return -ENODEV;
    if (!page || !length || length > BLK_MAX_IO_SIZE ||
        ((offset | length) & (BLK_SECTOR_SIZE - 1)) || offset >= PAGE_SIZE ||
        length > PAGE_SIZE - offset)
        return -EINVAL;
    if (sector >= dev->sectors ||
        length / BLK_SECTOR_SIZE > dev->sectors - sector)
        return -EINVAL;
    return 0;
}

int blk_read(struct blkdev *dev, uint64_t sector, struct page *page,
             size_t offset, size_t length)
{
    int ret = blk_validate(dev, sector, page, offset, length);

    return ret ? ret : dev->ops->read(dev, sector, page, offset, length);
}

int blk_write(struct blkdev *dev, uint64_t sector, struct page *page,
              size_t offset, size_t length)
{
    int ret = blk_validate(dev, sector, page, offset, length);

    return ret ? ret : dev->ops->write(dev, sector, page, offset, length);
}

int blk_flush(struct blkdev *dev)
{
    return dev ? dev->ops->flush(dev) : -ENODEV;
}
