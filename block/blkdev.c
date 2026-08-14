/* block/blkdev.c - block-device registration and lookup */

#include <nuvix/blkdev.h>
#include <nuvix/errno.h>

#define NR_BLOCK_DEVICES 32

static struct blkdev *dev_table[NR_BLOCK_DEVICES];

static int blk_mapping_resolve(struct page_mapping *mapping, uint64_t index,
				 bool create, uint64_t *block)
{
	(void)create;
	if (!mapping || !block)
		return -EINVAL;
	if (index > UINT32_MAX)
		return -EFBIG;
	*block = index;
	return 0;
}

static const struct page_mapping_ops blk_mapping_ops = {
	.resolve = blk_mapping_resolve,
};

int register_blkdev(struct blkdev *bdev)
{
	uint32_t major;
	if (!bdev)
		return -EINVAL;
	major = MAJOR(bdev->bd_dev);
	if (major >= NR_BLOCK_DEVICES)
		return -EINVAL;
	page_mapping_init(&bdev->bd_pages, bdev, bdev->bd_dev,
			  &blk_mapping_ops);
	dev_table[major] = bdev;
	return 0;
}

struct blkdev *lookup_blkdev(dev_t dev)
{
	uint32_t major = MAJOR(dev);
	if (major >= NR_BLOCK_DEVICES)
		return NULL;
	return dev_table[major];
}

struct page_mapping *blkdev_pages(dev_t dev)
{
	struct blkdev *bdev = lookup_blkdev(dev);
	if (!bdev)
		return NULL;
	if (!bdev->bd_pages.ops)
		page_mapping_init(&bdev->bd_pages, bdev, dev, &blk_mapping_ops);
	return &bdev->bd_pages;
}
