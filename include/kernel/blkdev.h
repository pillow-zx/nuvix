#ifndef _CUTEOS_KERNEL_BLKDEV_H
#define _CUTEOS_KERNEL_BLKDEV_H

/**
 * @file blkdev.h
 * @brief 块设备抽象层和 512-byte sector / 4 KiB block 合同。
 */

#include <kernel/page_mapping.h>
#include <kernel/types.h>
#include <kernel/compiler.h>
#include <uapi/stat.h>

/**
 * @def SECTOR_SIZE
 * @brief Block-device ABI sector size in bytes.
 */
#define SECTOR_SIZE 512u

/**
 * @def SECTOR_SHIFT
 * @brief log2(SECTOR_SIZE), used for byte/sector conversion.
 */
#define SECTOR_SHIFT 9

/**
 * @def BLOCK_SIZE
 * @brief Kernel page-cache block size in bytes.
 */
#define BLOCK_SIZE 4096u

/**
 * @def BLOCK_SECTORS
 * @brief Number of 512-byte sectors contained in one 4 KiB cache block.
 */
#define BLOCK_SECTORS (BLOCK_SIZE / SECTOR_SIZE)

struct blkdev;

/**
 * @struct block_device_operations
 * @brief Low-level sector I/O callbacks implemented by block drivers.
 *
 * @par Fields
 * - @c read_sectors: Read @p nsec sectors starting at @p sector into @p buf.
 * - @c write_sectors: Write @p nsec sectors starting at @p sector from @p buf.
 */
struct blkdev_ops {
	int (*read_sectors)(struct blkdev *bdev, void *buf, uint64_t sector,
			    uint32_t nsec);
	int (*write_sectors)(struct blkdev *bdev, const void *buf,
			     uint64_t sector, uint32_t nsec);
};

/**
 * @struct block_device
 * @brief Registered block device visible to VFS/filesystems by dev_t.
 *
 * @par Fields
 * - @c bd_dev: Linux-style device number.
 * - @c bd_sectors: Total capacity measured in 512-byte sectors.
 * - @c bd_ops: Driver I/O hooks.
 * - @c bd_private: Driver-private state.
 * - @c bd_pages: Raw 4 KiB page-cache mapping.
 */
struct blkdev {
	dev_t bd_dev;
	uint64_t bd_sectors;
	const struct blkdev_ops *bd_ops;
	void *bd_private;
	struct page_mapping bd_pages;
};

/**
 * @brief Register a block device by dev_t.
 * @param bdev Initialized block device.
 * @return 0 on success, or a negative errno.
 */
int register_blkdev(struct blkdev *bdev);

/**
 * @brief Lookup a registered block device.
 * @param dev Device number.
 * @return Matching block device, or NULL.
 */
__must_check
struct blkdev *lookup_blkdev(dev_t dev);

/**
 * @brief Return the raw page-cache mapping for a block device.
 * @param dev Device number.
 * @return Page mapping, or NULL when @p dev is unknown.
 */
__must_check
struct page_mapping *blkdev_pages(dev_t dev);

#endif
