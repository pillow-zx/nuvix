#ifndef _NUVIX_BLKDEV_H
#define _NUVIX_BLKDEV_H

#include <nuvix/ctypes.h>
#include <nuvix/layout.h>

#define BLK_SECTOR_SIZE 512
#define BLK_MAX_IO_SIZE PAGE_SIZE

struct page;
struct blkdev;

/* All operations are supplied; read/write receive already validated ranges. */
struct blkdev_ops {
    int (*read)(struct blkdev *dev, uint64_t sector, struct page *page,
                size_t offset, size_t length);
    int (*write)(struct blkdev *dev, uint64_t sector, struct page *page,
                 size_t offset, size_t length);
    int (*flush)(struct blkdev *dev);
};

/* Immutable after boot; the device and its ops remain alive permanently. */
struct blkdev {
    uint64_t sectors;
    const struct blkdev_ops *ops;
};

/* Published at boot after the root driver starts successfully; NULL until then. */
extern struct blkdev *root_bdev;

/*
 * Ordinary sleepable context, with no caller short locks/preempt-disable levels;
 * scheduler, local clock and timers must be ready. Sector units are 512 bytes.
 * page is a live, directly mapped order-0 allocation; the caller retains it
 * throughout the call. offset/length are sector-aligned and within that page,
 * length is nonzero and at most BLK_MAX_IO_SIZE. No page reference is taken.
 * Read buffers are exclusively owned; write data stays unchanged until return.
 * Returns 0 for the entire transfer or a negative errno. An error may still
 * leave some read data changed or some disk sectors written; no automatic retry.
 */
int blk_read(struct blkdev *dev, uint64_t sector, struct page *page,
             size_t offset, size_t length);
int blk_write(struct blkdev *dev, uint64_t sector, struct page *page,
              size_t offset, size_t length);

/*
 * Same execution context as read/write. First wait for the writes that need
 * persistence; this flush does not wait for other in-flight requests or exclude
 * new submissions. Returns 0 or a negative errno.
 */
int blk_flush(struct blkdev *dev);

#endif /* _NUVIX_BLKDEV_H */
