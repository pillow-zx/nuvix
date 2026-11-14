#include <nuvix/virtio.h>
#include <nuvix/virtqueue.h>
#include <nuvix/assert.h>
#include <nuvix/cleanup.h>
#include <nuvix/dtb.h>
#include <nuvix/errno.h>
#include <nuvix/irq.h>
#include <nuvix/libfdt.h>
#include <nuvix/math.h>
#include <nuvix/module.h>
#include <nuvix/page.h>
#include <nuvix/pgtable.h>
#include <nuvix/slab.h>
#include <asm/io.h>

#define MMIO_MAGIC                0x000
#define MMIO_VERSION              0x004
#define MMIO_DEVICE_ID            0x008
#define MMIO_DEVICE_FEATURES      0x010
#define MMIO_DEVICE_FEATURE_SEL   0x014
#define MMIO_DRIVER_FEATURES      0x020
#define MMIO_DRIVER_FEATURE_SEL   0x024
#define MMIO_QUEUE_SEL            0x030
#define MMIO_QUEUE_NUM_MAX        0x034
#define MMIO_QUEUE_NUM            0x038
#define MMIO_QUEUE_READY          0x044
#define MMIO_QUEUE_NOTIFY         0x050
#define MMIO_INTERRUPT_STATUS     0x060
#define MMIO_INTERRUPT_ACK        0x064
#define MMIO_STATUS               0x070
#define MMIO_QUEUE_DESC           0x080
#define MMIO_QUEUE_AVAIL          0x090
#define MMIO_QUEUE_USED           0x0a0
#define MMIO_CONFIG_GENERATION    0x0fc
#define MMIO_CONFIG               0x100

#define VIRTIO_MMIO_MAGIC         0x74726976
#define VIRTIO_MMIO_VERSION       2
#define VIRTIO_STATUS_ACK         1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_NEEDS_RESET 64
#define VIRTIO_STATUS_FAILED      128
#define VIRTIO_F_VERSION_1        (1ULL << 32)
#define VIRTIO_IRQ_QUEUE          1
#define VIRTIO_IRQ_CONFIG         2
#define VIRTIO_RESET_POLLS        100000
#define MMIO_CONFIG_POLLS         100000

static vaddr_t next_mmio = VIRTIO_MMIO_VBASE;

static uint32_t mmio_read(const struct virtio_dev *dev, unsigned int offset)
{
    return readl((const volatile char *)dev->base + offset);
}

static void mmio_write(struct virtio_dev *dev, unsigned int offset,
                       uint32_t value)
{
    writel(value, (volatile char *)dev->base + offset);
}

static void mmio_write_address(struct virtio_dev *dev, unsigned int offset,
                               dma_t addr)
{
    mmio_write(dev, offset, (uint32_t)addr);
    mmio_write(dev, offset + sizeof(uint32_t), (uint32_t)(addr >> 32));
}

static int mmio_reg(const struct dtb_range *range, void *arg)
{
    struct dtb_range *reg = arg;

    if (reg->size)
        return -EINVAL;
    *reg = *range;
    return 0;
}

static int mmio_map(int node, struct virtio_dev *dev)
{
    struct dtb_range reg = {0};
    size_t offset, bytes;
    vaddr_t vaddr;
    int ret;

    ret = dtb_scan_reg(node, mmio_reg, &reg);
    if (ret)
        return ret;
    if ((reg.base & (sizeof(uint32_t) - 1)) || reg.size < MMIO_CONFIG)
        return -EINVAL;
    offset = reg.base & (PAGE_SIZE - 1);
    if (reg.size > VIRTIO_MMIO_SIZE - offset)
        return -E2BIG;
    bytes = ALIGN_UP((size_t)reg.size + offset, (size_t)PAGE_SIZE);
    if (bytes > VIRTIO_MMIO_SIZE - (next_mmio - VIRTIO_MMIO_VBASE))
        return -ENOSPC;
    vaddr = next_mmio;
    next_mmio += bytes;
    ret = pgtable_map_io(vaddr, reg.base - offset, bytes);
    if (ret)
        return ret;
    dev->base = (volatile void *)(vaddr + offset);
    dev->config_size = reg.size - MMIO_CONFIG;
    return 0;
}

static bool virtio_match(const struct device *dev, const struct driver *drv)
{
    return to_virtio_device_const(dev)->device_id ==
           to_virtio_driver(drv)->device_id;
}

static int virtio_reset(struct virtio_dev *dev)
{
    mmio_write(dev, MMIO_STATUS, 0);
    for (unsigned int i = 0; i < VIRTIO_RESET_POLLS; ++i)
        if (!mmio_read(dev, MMIO_STATUS))
            return 0;
    return -ETIMEDOUT;
}

static int virtio_negotiate(struct virtio_dev *dev)
{
    const struct virtio_driver *drv = to_virtio_driver(dev->dev.drv);
    uint64_t required = drv->required_features | VIRTIO_F_VERSION_1;
    uint64_t features;
    uint32_t status;

    mmio_write(dev, MMIO_DEVICE_FEATURE_SEL, 0);
    features = mmio_read(dev, MMIO_DEVICE_FEATURES);
    mmio_write(dev, MMIO_DEVICE_FEATURE_SEL, 1);
    features |= (uint64_t)mmio_read(dev, MMIO_DEVICE_FEATURES) << 32;
    if ((features & required) != required)
        return -EOPNOTSUPP;
    dev->features = features & (required | drv->features);
    mmio_write(dev, MMIO_DRIVER_FEATURE_SEL, 0);
    mmio_write(dev, MMIO_DRIVER_FEATURES, (uint32_t)dev->features);
    mmio_write(dev, MMIO_DRIVER_FEATURE_SEL, 1);
    mmio_write(dev, MMIO_DRIVER_FEATURES, (uint32_t)(dev->features >> 32));
    status = mmio_read(dev, MMIO_STATUS) | VIRTIO_STATUS_FEATURES_OK;
    mmio_write(dev, MMIO_STATUS, status);
    status = mmio_read(dev, MMIO_STATUS);
    if (!(status & VIRTIO_STATUS_FEATURES_OK) ||
        (status & (VIRTIO_STATUS_NEEDS_RESET | VIRTIO_STATUS_FAILED)))
        return -EIO;
    return 0;
}

static void virtio_irq(void *data)
{
    struct virtio_dev *dev = data;
    const struct virtio_driver *drv = to_virtio_driver(dev->dev.drv);
    uint32_t events = mmio_read(dev, MMIO_INTERRUPT_STATUS) &
                      (VIRTIO_IRQ_QUEUE | VIRTIO_IRQ_CONFIG);

    mmio_write(dev, MMIO_INTERRUPT_ACK, events);
    if (events & VIRTIO_IRQ_CONFIG) {
        if (!drv->config_changed)
            panic("%s: unsupported virtio configuration change\n",
                  drv->drv.name);
        drv->config_changed(dev);
    }
    if (events & VIRTIO_IRQ_QUEUE)
        for (unsigned int i = 0; i < dev->nvqs; ++i)
            if (dev->vqs[i]->callback)
                dev->vqs[i]->callback(dev->vqs[i]);
}

static int virtio_probe(struct device *device)
{
    struct virtio_dev *dev = to_virtio_device(device);
    bool irq_registered = false;
    uint32_t status;
    int ret;

    ret = virtio_reset(dev);
    if (ret)
        goto fail;
    mmio_write(dev, MMIO_STATUS, VIRTIO_STATUS_ACK);
    mmio_write(dev, MMIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    ret = virtio_negotiate(dev);
    if (ret)
        goto fail;
    ret = irq_register(dev->irq, virtio_irq, dev);
    if (ret)
        goto fail;
    irq_registered = true;
    ret = device->drv->probe(device);
    if (!ret)
        return 0;
fail:
    status = mmio_read(dev, MMIO_STATUS);
    Assert(!(status & VIRTIO_STATUS_DRIVER_OK),
           "virtio probe failed after device start\n");
    if (irq_registered)
        Assert(!irq_unregister(dev->irq), "virtio IRQ unregister failed\n");
    mmio_write(dev, MMIO_STATUS, status | VIRTIO_STATUS_FAILED);
    Assert(!virtio_reset(dev), "virtio reset did not complete\n");
    for (unsigned int i = 0; i < dev->nvqs; ++i)
        vq_destroy(dev->vqs[i]);
    return ret;
}

const struct bus virtio_bus = {
    .name = "virtio",
    .match = virtio_match,
    .probe = virtio_probe,
};

int vq_create(struct virtio_dev *dev, unsigned int num,
              void (*callback)(struct virtqueue *))
{
    struct virtqueue *vq __free(kfree) = NULL;
    size_t avail_offset, used_offset;
    dma_t dma_addr;

    mmio_write(dev, MMIO_QUEUE_SEL, dev->nvqs);
    if (num > mmio_read(dev, MMIO_QUEUE_NUM_MAX))
        return -ENOSPC;
    vq = kzalloc(sizeof(*vq) + num * sizeof(vq->cookies[0]));
    if (!vq)
        return -ENOMEM;
    avail_offset = num * sizeof(struct vring_desc);
    used_offset = ALIGN_UP(avail_offset + sizeof(struct vring_avail) +
                               (num + 1) * sizeof(uint16_t),
                           (size_t)ALIGNOF(struct vring_used));
    vq->desc = dma_alloc_coherent(0, &dma_addr);
    if (!vq->desc)
        return -ENOMEM;
    vq->dev = dev;
    vq->index = dev->nvqs;
    vq->num = num;
    vq->callback = callback;
    spin_lock_init(&vq->lock);
    vq->avail = (void *)((char *)vq->desc + avail_offset);
    vq->used = (void *)((char *)vq->desc + used_offset);
    vq->num_free = num;
    for (unsigned int i = 0; i < num; ++i)
        vq->desc[i].next = i + 1;
    mmio_write(dev, MMIO_QUEUE_NUM, num);
    mmio_write_address(dev, MMIO_QUEUE_DESC, dma_addr);
    mmio_write_address(dev, MMIO_QUEUE_AVAIL, dma_addr + avail_offset);
    mmio_write_address(dev, MMIO_QUEUE_USED, dma_addr + used_offset);
    dma_wmb();
    mmio_write(dev, MMIO_QUEUE_READY, 1);
    dev->vqs[dev->nvqs++] = move(vq);
    return 0;
}

void vq_kick(struct virtqueue *vq)
{
    unsigned long flags;

    spin_lock_irqsave(&vq->lock, flags);
    dma_wmb();
    mmio_write(vq->dev, MMIO_QUEUE_NOTIFY, vq->index);
    spin_unlock_irqrestore(&vq->lock, flags);
}

int virtio_dev_ready(struct virtio_dev *dev)
{
    uint32_t status = mmio_read(dev, MMIO_STATUS);
    int ret;

    if (status & (VIRTIO_STATUS_NEEDS_RESET | VIRTIO_STATUS_FAILED))
        return -EIO;
    ret = irq_enable(dev->irq);
    if (ret)
        return ret;
    mmio_write(dev, MMIO_STATUS, status | VIRTIO_STATUS_DRIVER_OK);
    return 0;
}

static int config_read(struct virtio_dev *dev, unsigned int offset, void *buf,
                       size_t length)
{
    if (offset > dev->config_size || length > dev->config_size - offset)
        return -EINVAL;
    for (unsigned int i = 0; i < MMIO_CONFIG_POLLS; ++i) {
        uint32_t generation = mmio_read(dev, MMIO_CONFIG_GENERATION);
        uint32_t *dst = buf;

        for (size_t pos = 0; pos < length; pos += sizeof(*dst))
            *dst++ = mmio_read(dev, MMIO_CONFIG + offset + pos);
        if (generation == mmio_read(dev, MMIO_CONFIG_GENERATION))
            return 0;
    }
    return -EAGAIN;
}

int virtio_config_read32(struct virtio_dev *dev, unsigned int offset,
                         uint32_t *value)
{
    uint32_t result;
    int ret = config_read(dev, offset, &result, sizeof(result));

    if (!ret)
        *value = result;
    return ret;
}

int virtio_config_read64(struct virtio_dev *dev, unsigned int offset,
                         uint64_t *value)
{
    uint64_t result;
    int ret = config_read(dev, offset, &result, sizeof(result));

    if (!ret)
        *value = result;
    return ret;
}

int virtio_config_write32(struct virtio_dev *dev, unsigned int offset,
                          uint32_t value)
{
    if (offset > dev->config_size || sizeof(value) > dev->config_size - offset)
        return -EINVAL;
    mmio_write(dev, MMIO_CONFIG + offset, value);
    return 0;
}

static int virtio_mmio_init(void)
{
    const void *blob = dtb.blob;
    int node = -1, ret;

    while ((node = fdt_node_offset_by_compatible(blob, node, "virtio,mmio")) >=
           0) {
        struct virtio_dev *dev __free(kfree) = NULL;

        ret = dtb_node_available(node);
        if (ret < 0)
            return ret;
        if (!ret)
            continue;
        dev = kzalloc(sizeof(*dev));
        if (!dev)
            return -ENOMEM;
        ret = mmio_map(node, dev);
        if (ret)
            return ret;
        if (mmio_read(dev, MMIO_MAGIC) != VIRTIO_MMIO_MAGIC ||
            mmio_read(dev, MMIO_VERSION) != VIRTIO_MMIO_VERSION)
            continue;
        dev->device_id = mmio_read(dev, MMIO_DEVICE_ID);
        if (!dev->device_id)
            continue;
        ret = irq_parse(node, &dev->irq);
        if (ret)
            return ret;
        dev->dev.name = fdt_get_name(blob, node, NULL);
        dev->dev.bus = &virtio_bus;
        device_register(&dev->dev);
        keep(dev);
    }
    return node == -FDT_ERR_NOTFOUND ? 0 : -EINVAL;
}

MODULE_INIT(virtio_mmio_init, 71);
