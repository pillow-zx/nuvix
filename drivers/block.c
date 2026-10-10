#include <nuvix/blkdev.h>
#include <nuvix/errno.h>

struct block_device *root_bdev;

static int blk_validate(const struct block_device *dev, uint64_t sector,
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

int blk_read(struct block_device *dev, uint64_t sector, struct page *page,
             size_t offset, size_t length)
{
    int ret = blk_validate(dev, sector, page, offset, length);

    return ret ? ret : dev->ops->read(dev, sector, page, offset, length);
}

int blk_write(struct block_device *dev, uint64_t sector, struct page *page,
              size_t offset, size_t length)
{
    int ret = blk_validate(dev, sector, page, offset, length);

    return ret ? ret : dev->ops->write(dev, sector, page, offset, length);
}

int blk_flush(struct block_device *dev)
{
    return dev ? dev->ops->flush(dev) : -ENODEV;
}
