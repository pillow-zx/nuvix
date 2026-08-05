#ifndef _CUTEOS_DRIVERS_VIRTIO_BLK_H
#define _CUTEOS_DRIVERS_VIRTIO_BLK_H

/**
 * @file virtio_blk.h
 * @brief virtio-blk device-number contract and initialization API.
 */

#include <kernel/blkdev.h>

/**
 * @def VIRTIO_BLK_MAJOR
 * @brief Linux-compatible major number used for virtio block devices.
 */
#define VIRTIO_BLK_MAJOR 8U

/**
 * @def ROOT_DEV
 * @brief Root filesystem block device, currently virtio-blk disk 0.
 */
#define ROOT_DEV MKDEV(VIRTIO_BLK_MAJOR, 0)

/**
 * @brief Discover and register the QEMU virtio-blk root device.
 */
void virtio_blk_init(void);

#endif
