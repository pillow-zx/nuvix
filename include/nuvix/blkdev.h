#ifndef _NUVIX_BLKDEV_H
#define _NUVIX_BLKDEV_H

#include <nuvix/ctypes.h>
#include <nuvix/layout.h>
#include <nuvix/list.h>

#define BLK_SECTOR_SIZE 512
#define BLK_MAX_IO_SIZE PAGE_SIZE

struct page;
struct blkdev;
struct file_operations;

struct blkdev_ops {
    int (*claim)(struct blkdev *dev, unsigned int flags);
    void (*release)(struct blkdev *dev);
    int (*read)(struct blkdev *dev, uint64_t sector, struct page *page, size_t offset, size_t length);
    int (*write)(struct blkdev *dev, uint64_t sector, struct page *page, size_t offset, size_t length);
    int (*flush)(struct blkdev *dev);
};

struct blkdev {
    dev_t dev;
    struct list_head node;
    uint64_t sectors;
    const struct blkdev_ops *ops;
    const struct file_operations *fops;
};

extern const struct file_operations blkdev_file_operations;

void blkdev_register(struct blkdev *dev);
struct blkdev *blkdev_find(dev_t dev);
struct blkdev *blkdev_first(void);

int blk_read(struct blkdev *dev, uint64_t sector, struct page *page, size_t offset, size_t length);
int blk_write(struct blkdev *dev, uint64_t sector, struct page *page, size_t offset, size_t length);
int blk_flush(struct blkdev *dev);

#endif /* _NUVIX_BLKDEV_H */
