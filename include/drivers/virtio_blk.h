#ifndef _NUVIX_DRIVERS_VIRTIO_BLK_H
#define _NUVIX_DRIVERS_VIRTIO_BLK_H

/**
 * @file virtio_blk.h
 * @brief virtio-blk device-number contract and initialization API.
 */

#include <nuvix/blkdev.h>

/**
 * @def VIRTIO_BLK_MAJOR
 * @brief Linux-compatible major number used for virtio block devices.
 */
#define VIRTIO_BLK_MAJOR 8U

vaddr_t virtio_blk_probe(int node);

dev_t virtio_blk_init(vaddr_t base, unsigned irq);

void virtio_blk_watchdog_thread(void *arg);

#endif
