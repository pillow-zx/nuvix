#include <nuvix/virtio.h>
#include <nuvix/virtqueue.h>
#include <nuvix/cleanup.h>
#include <nuvix/dtb.h>
#include <nuvix/errno.h>
#include <nuvix/irq.h>
#include <nuvix/libfdt.h>
#include <nuvix/math.h>
#include <nuvix/pgtable.h>
#include <nuvix/string.h>
#include <nuvix/slab.h>
#include <asm/io.h>

#define MMIO_MAGIC              0x000
#define MMIO_VERSION            0x004
#define MMIO_DEVICE_ID          0x008
#define MMIO_VENDOR_ID          0x00c
#define MMIO_DEVICE_FEATURES    0x010
#define MMIO_DEVICE_FEATURE_SEL 0x014
#define MMIO_DRIVER_FEATURES    0x020
#define MMIO_DRIVER_FEATURE_SEL 0x024
#define MMIO_QUEUE_SEL          0x030
#define MMIO_QUEUE_NUM_MAX      0x034
#define MMIO_QUEUE_NUM          0x038
#define MMIO_QUEUE_READY        0x044
#define MMIO_QUEUE_NOTIFY       0x050
#define MMIO_INTERRUPT_STATUS   0x060
#define MMIO_INTERRUPT_ACK      0x064
#define MMIO_STATUS             0x070
#define MMIO_QUEUE_DESC         0x080
#define MMIO_QUEUE_AVAIL        0x090
#define MMIO_QUEUE_USED         0x0a0
#define MMIO_CONFIG_GENERATION  0x0fc
#define MMIO_CONFIG             0x100

#define VIRTIO_MMIO_MAGIC       0x74726976
#define VIRTIO_MMIO_VERSION     2
#define MMIO_CONFIG_POLLS       100000

struct virtio_mmio {
    struct virtio_dev vdev;
    volatile void *base;
    size_t reg_size;
};

static vaddr_t next_mmio = VIRTIO_MMIO_VBASE;

static struct virtio_mmio *to_virtio_mmio(struct virtio_dev *dev)
{
    return container_of(dev, struct virtio_mmio, vdev);
}

static uint32_t mmio_read(const struct virtio_mmio *dev, unsigned int offset)
{
    return readl((const volatile char *)dev->base + offset);
}

static void mmio_write(struct virtio_mmio *dev, unsigned int offset,
                       uint32_t value)
{
    writel(value, (volatile char *)dev->base + offset);
}

static void mmio_write_address(struct virtio_mmio *dev, unsigned int offset,
                               dma_addr_t addr)
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

static int mmio_map(int node, struct virtio_mmio *mmio)
{
    struct dtb_range reg = {0};
    uint64_t paddr, size;
    size_t offset, bytes;
    vaddr_t vaddr;
    int ret;

    ret = dtb_scan_reg(node, mmio_reg, &reg);
    if (ret)
        return ret;
    paddr = reg.base;
    size = reg.size;
    if ((paddr & (sizeof(uint32_t) - 1)) || size < MMIO_CONFIG)
        return -EINVAL;
    offset = paddr & (PAGE_SIZE - 1);
    if (size > VIRTIO_MMIO_SIZE - offset)
        return -E2BIG;
    bytes = ALIGN_UP((size_t)size + offset, (size_t)PAGE_SIZE);
    if (bytes > VIRTIO_MMIO_SIZE - (next_mmio - VIRTIO_MMIO_VBASE))
        return -ENOSPC;
    vaddr = next_mmio;
    /* Reserve even on failure: pgtable_map_io may leave partial mappings. */
    next_mmio += bytes;
    ret = pgtable_map_io(vaddr, paddr - offset, bytes);
    if (ret)
        return ret;
    mmio->base = (volatile void *)(vaddr + offset);
    mmio->reg_size = size;
    return 0;
}

static uint32_t mmio_get_status(struct virtio_dev *dev)
{
    return mmio_read(to_virtio_mmio(dev), MMIO_STATUS);
}

static void mmio_set_status(struct virtio_dev *dev, uint32_t status)
{
    mmio_write(to_virtio_mmio(dev), MMIO_STATUS, status);
}

static uint64_t mmio_get_features(struct virtio_dev *dev)
{
    struct virtio_mmio *mmio = to_virtio_mmio(dev);
    uint64_t features;

    mmio_write(mmio, MMIO_DEVICE_FEATURE_SEL, 0);
    features = mmio_read(mmio, MMIO_DEVICE_FEATURES);
    mmio_write(mmio, MMIO_DEVICE_FEATURE_SEL, 1);
    return features | (uint64_t)mmio_read(mmio, MMIO_DEVICE_FEATURES) << 32;
}

static void mmio_set_features(struct virtio_dev *dev, uint64_t features)
{
    struct virtio_mmio *mmio = to_virtio_mmio(dev);

    mmio_write(mmio, MMIO_DRIVER_FEATURE_SEL, 0);
    mmio_write(mmio, MMIO_DRIVER_FEATURES, (uint32_t)features);
    mmio_write(mmio, MMIO_DRIVER_FEATURE_SEL, 1);
    mmio_write(mmio, MMIO_DRIVER_FEATURES, (uint32_t)(features >> 32));
}

static int mmio_config_read(struct virtio_dev *dev, unsigned int offset,
                            void *buf, size_t length)
{
    struct virtio_mmio *mmio = to_virtio_mmio(dev);
    size_t available = mmio->reg_size - MMIO_CONFIG;

    if (offset > available || length > available - offset)
        return -EINVAL;
    for (unsigned int i = 0; i < MMIO_CONFIG_POLLS; ++i) {
        uint32_t generation = mmio_read(mmio, MMIO_CONFIG_GENERATION);
        unsigned int pos = MMIO_CONFIG + offset;
        size_t remaining = length;
        char *dst = buf;

        while (remaining) {
            if (!(pos & (sizeof(uint32_t) - 1)) &&
                remaining >= sizeof(uint32_t)) {
                uint32_t value = mmio_read(mmio, pos);

                memcpy(dst, &value, sizeof(value));
                dst += sizeof(value);
                pos += sizeof(value);
                remaining -= sizeof(value);
            } else {
                *dst++ = readb((const volatile char *)mmio->base + pos++);
                --remaining;
            }
        }
        if (generation == mmio_read(mmio, MMIO_CONFIG_GENERATION))
            return 0;
    }
    return -EAGAIN;
}

static unsigned int mmio_queue_max(struct virtio_dev *dev, unsigned int index)
{
    struct virtio_mmio *mmio = to_virtio_mmio(dev);

    mmio_write(mmio, MMIO_QUEUE_SEL, index);
    return mmio_read(mmio, MMIO_QUEUE_NUM_MAX);
}

static int mmio_setup_vq(struct virtio_dev *dev, struct virtqueue *vq)
{
    struct virtio_mmio *mmio = to_virtio_mmio(dev);

    mmio_write(mmio, MMIO_QUEUE_SEL, vq->index);
    mmio_write(mmio, MMIO_QUEUE_NUM, vq->num);
    mmio_write_address(mmio, MMIO_QUEUE_DESC, vq->desc_dma);
    mmio_write_address(mmio, MMIO_QUEUE_AVAIL, vq->avail_dma);
    mmio_write_address(mmio, MMIO_QUEUE_USED, vq->used_dma);
    dma_wmb();
    mmio_write(mmio, MMIO_QUEUE_READY, 1);
    return 0;
}

static void mmio_notify(struct virtio_dev *dev, struct virtqueue *vq)
{
    mmio_write(to_virtio_mmio(dev), MMIO_QUEUE_NOTIFY, vq->index);
}

static uint32_t mmio_ack_irq(struct virtio_dev *dev)
{
    struct virtio_mmio *mmio = to_virtio_mmio(dev);
    uint32_t events = mmio_read(mmio, MMIO_INTERRUPT_STATUS) &
                      (VIRTIO_IRQ_QUEUE | VIRTIO_IRQ_CONFIG);

    /* ACK before callbacks so a concurrent completion can notify again. */
    if (events)
        mmio_write(mmio, MMIO_INTERRUPT_ACK, events);
    return events;
}

static const struct virtio_transport_ops mmio_ops = {
    .get_status = mmio_get_status,
    .set_status = mmio_set_status,
    .get_features = mmio_get_features,
    .set_features = mmio_set_features,
    .config_read = mmio_config_read,
    .queue_max = mmio_queue_max,
    .setup_vq = mmio_setup_vq,
    .notify = mmio_notify,
    .ack_irq = mmio_ack_irq,
};

int virtio_mmio_scan(void)
{
    const void *blob = dtb.blob;
    int node = -1, ret;

    while ((node = fdt_node_offset_by_compatible(blob, node, "virtio,mmio")) >=
           0) {
        struct virtio_mmio *mmio __free(kfree) = NULL;

        ret = dtb_node_available(node);
        if (ret < 0)
            return ret;
        if (!ret)
            continue;
        mmio = kzalloc(sizeof(*mmio));
        if (!mmio)
            return -ENOMEM;
        ret = mmio_map(node, mmio);
        if (ret)
            return ret;
        if (mmio_read(mmio, MMIO_MAGIC) != VIRTIO_MMIO_MAGIC ||
            mmio_read(mmio, MMIO_VERSION) != VIRTIO_MMIO_VERSION)
            continue;
        mmio->vdev.device_id = mmio_read(mmio, MMIO_DEVICE_ID);
        if (!mmio->vdev.device_id)
            continue;
        mmio->vdev.vendor_id = mmio_read(mmio, MMIO_VENDOR_ID);
        ret = irq_parse(node, &mmio->vdev.irq);
        if (ret)
            return ret;
        mmio->vdev.ops = &mmio_ops;
        mmio->vdev.dev.name = fdt_get_name(blob, node, NULL);
        mmio->vdev.dev.bus = &virtio_bus;
        ret = device_register(&mmio->vdev.dev);
        if (ret)
            return ret;
        keep(mmio);
    }
    return node == -FDT_ERR_NOTFOUND ? 0 : -EINVAL;
}
