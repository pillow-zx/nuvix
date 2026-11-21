#define pr_fmt(fmt) "virtblk: " fmt

#include <nuvix/blkdev.h>
#include <nuvix/module.h>
#include <nuvix/virtio.h>
#include <nuvix/virtqueue.h>
#include <nuvix/assert.h>
#include <nuvix/completion.h>
#include <nuvix/dma.h>
#include <nuvix/errno.h>
#include <nuvix/macro.h>
#include <nuvix/printk.h>
#include <nuvix/tick.h>
#include <nuvix/wait.h>
#include <nuvix/slab.h>

#define VIRTIO_BLK_F_SIZE_MAX (1ULL << 1)
#define VIRTIO_BLK_F_SEG_MAX  (1ULL << 2)
#define VIRTIO_BLK_F_RO       (1ULL << 5)
#define VIRTIO_BLK_F_FLUSH    (1ULL << 9)

#define VIRTIO_BLK_CAPACITY   0
#define VIRTIO_BLK_SIZE_MAX   8
#define VIRTIO_BLK_SEG_MAX    12

#define VIRTIO_BLK_T_IN       0
#define VIRTIO_BLK_T_OUT      1
#define VIRTIO_BLK_T_FLUSH    4
#define VIRTIO_BLK_S_OK       0
#define VIRTIO_BLK_S_IOERR    1
#define VIRTIO_BLK_S_UNSUPP   2

#define VIRTIO_BLK_QUEUE_SIZE 64
#define VIRTIO_BLK_SLOTS      16
#define VIRTIO_BLK_TIMEOUT_NS (30ULL * NSEC_PER_SEC)

struct virtblk_hdr {
    uint32_t type;
    uint8_t reserved[4];
    uint64_t sector;
};

struct virtblk_wire {
    struct virtblk_hdr header;
    uint8_t status;
};

struct virtblk_req {
    bool busy;
    struct completion completion;
    struct virtblk_wire *wire;
    dma_t wire_dma;
    uint32_t expected_written;
    int result;
};

struct virtio_blk {
    struct blkdev bdev;
    struct virtqueue *vq;
    struct wait_queue available;
    struct virtblk_req requests[VIRTIO_BLK_SLOTS];
};

static struct virtblk_req *virtblk_reserve(struct virtio_blk *blk)
{
    for (unsigned int i = 0; i < VIRTIO_BLK_SLOTS; ++i) {
        struct virtblk_req *req = &blk->requests[i];

        if (!req->busy) {
            req->busy = true;
            return req;
        }
    }
    return NULL;
}

static void virtblk_release(struct virtio_blk *blk, struct virtblk_req *req)
{
    unsigned long flags;

    spin_lock_irqsave(&blk->available.lock, flags);
    req->busy = false;
    spin_unlock_irqrestore(&blk->available.lock, flags);
    wake_up_one(&blk->available);
}

static int virtblk_submit(struct virtio_blk *blk, uint32_t type,
                          uint64_t sector, struct page *page, size_t offset,
                          size_t length)
{
    struct virtblk_req *req;
    struct vq_sg bufs[3];
    enum dma_direction direction =
        type == VIRTIO_BLK_T_IN ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
    dma_t data_dma;
    unsigned int num_out = 1, num_in = 1, status_index = 1;
    unsigned long flags;
    uint64_t deadline;
    int ret;

    wait_event(&blk->available, (req = virtblk_reserve(blk)) != NULL);
    if (length) {
        ret = dma_map_page(page, offset, length, direction, &data_dma);
        if (ret)
            goto release;
        bufs[1] = (struct vq_sg){data_dma, length};
        status_index = 2;
        if (type == VIRTIO_BLK_T_IN)
            ++num_in;
        else
            ++num_out;
    }
    req->wire->header = (struct virtblk_hdr){
        .type = type,
        .sector = sector,
    };
    req->wire->status = UINT8_MAX;
    req->expected_written = type == VIRTIO_BLK_T_IN ? length + 1 : 1;
    bufs[0] = (struct vq_sg){req->wire_dma, sizeof(req->wire->header)};
    bufs[status_index] = (struct vq_sg){
        req->wire_dma + offsetof(struct virtblk_wire, status), 1};
    ret = time_deadline_from_ns(VIRTIO_BLK_TIMEOUT_NS, &deadline);
    if (ret)
        goto unmap;

    spin_lock_irqsave(&blk->available.lock, flags);
    ret = vq_add(blk->vq, bufs, num_out, num_in, req);
    Assert(!ret, "request slot lacked virtqueue capacity (%d)\n", ret);
    vq_kick(blk->vq);
    spin_unlock_irqrestore(&blk->available.lock, flags);

    ret = wait_for_completion_until(&req->completion, deadline);
    Assert(!ret, "request type %u sector %llu failed to complete (%d)\n", type,
           (unsigned long long)sector, ret);
    ret = req->result;
unmap:
    if (length)
        dma_unmap_page(data_dma, length, direction);
release:
    virtblk_release(blk, req);
    return ret;
}

static int virtblk_read(struct blkdev *dev, uint64_t sector, struct page *page,
                        size_t offset, size_t length)
{
    struct virtio_blk *blk = container_of(dev, struct virtio_blk, bdev);

    return virtblk_submit(blk, VIRTIO_BLK_T_IN, sector, page, offset, length);
}

static int virtblk_write(struct blkdev *dev, uint64_t sector, struct page *page,
                         size_t offset, size_t length)
{
    struct virtio_blk *blk = container_of(dev, struct virtio_blk, bdev);

    return virtblk_submit(blk, VIRTIO_BLK_T_OUT, sector, page, offset, length);
}

static int virtblk_flush(struct blkdev *dev)
{
    struct virtio_blk *blk = container_of(dev, struct virtio_blk, bdev);

    return virtblk_submit(blk, VIRTIO_BLK_T_FLUSH, 0, NULL, 0, 0);
}

static const struct blkdev_ops virtblk_ops = {
    .read = virtblk_read,
    .write = virtblk_write,
    .flush = virtblk_flush,
};

static void virtblk_done(struct virtqueue *vq)
{
    struct virtio_blk *blk = vq->dev->dev.drvdata;
    struct virtblk_req *completed[VIRTIO_BLK_SLOTS];
    struct virtblk_req *req;
    unsigned int count = 0;
    unsigned long flags;
    uint32_t written;

    spin_lock_irqsave(&blk->available.lock, flags);
    while ((req = vq_get_buf(vq, &written))) {
        if (written != req->expected_written) {
            req->result = -EIO;
        } else {
            switch (READ_ONCE(req->wire->status)) {
            case VIRTIO_BLK_S_OK:
                req->result = 0;
                break;
            case VIRTIO_BLK_S_UNSUPP:
                req->result = -EOPNOTSUPP;
                break;
            default:
                req->result = -EIO;
                break;
            }
        }
        completed[count++] = req;
    }
    spin_unlock_irqrestore(&blk->available.lock, flags);
    for (unsigned int i = 0; i < count; ++i)
        complete(&completed[i]->completion);
}

static int virtblk_probe(struct device *device)
{
    struct virtio_dev *dev = to_virtio_device(device);
    struct virtio_blk *blk __free(kfree) = NULL;
    struct virtblk_wire *wire;
    dma_t wire_dma;
    uint32_t limit;
    int ret;

    if (dev->features & VIRTIO_BLK_F_RO)
        return -EROFS;
    blk = kzalloc(sizeof(*blk));
    if (!blk)
        return -ENOMEM;
    ret = virtio_config_read64(dev, VIRTIO_BLK_CAPACITY, &blk->bdev.sectors);
    if (ret)
        return ret;
    if (!blk->bdev.sectors)
        return -ENODEV;
    if (dev->features & VIRTIO_BLK_F_SIZE_MAX) {
        ret = virtio_config_read32(dev, VIRTIO_BLK_SIZE_MAX, &limit);
        if (ret)
            return ret;
        if (limit < BLK_MAX_IO_SIZE)
            return -EOPNOTSUPP;
    }
    if (dev->features & VIRTIO_BLK_F_SEG_MAX) {
        ret = virtio_config_read32(dev, VIRTIO_BLK_SEG_MAX, &limit);
        if (ret)
            return ret;
        if (!limit)
            return -EOPNOTSUPP;
    }
    ret = vq_create(dev, VIRTIO_BLK_QUEUE_SIZE, virtblk_done);
    if (ret)
        return ret;
    wait_queue_init(&blk->available);
    wire = dma_alloc_coherent(0, &wire_dma);
    if (!wire)
        return -ENOMEM;
    for (unsigned int i = 0; i < VIRTIO_BLK_SLOTS; ++i) {
        init_completion(&blk->requests[i].completion);
        blk->requests[i].wire = &wire[i];
        blk->requests[i].wire_dma = wire_dma + i * sizeof(*wire);
    }
    blk->vq = dev->vqs[0];
    blk->bdev.ops = &virtblk_ops;
    device->drvdata = blk;
    ret = virtio_dev_ready(dev);
    if (ret) {
        device->drvdata = NULL;
        dma_free_coherent(wire, 0);
        return ret;
    }
    if (!root_bdev)
        root_bdev = &blk->bdev;
    pr_info("device: %llu sectors, queue=%u, requests=%u\n",
            (unsigned long long)blk->bdev.sectors, VIRTIO_BLK_QUEUE_SIZE,
            VIRTIO_BLK_SLOTS);
    keep(blk);
    return 0;
}

static struct virtio_driver virtblk_driver = {
    .drv =
        {
            .name = "virtio-blk",
            .bus = &virtio_bus,
            .probe = virtblk_probe,
        },
    .device_id = VIRTIO_ID_BLOCK,
    .required_features = VIRTIO_BLK_F_FLUSH,
    .features = VIRTIO_BLK_F_SIZE_MAX | VIRTIO_BLK_F_SEG_MAX | VIRTIO_BLK_F_RO,
};

MODULE_DRIVER_INIT(virtblk, 81);
