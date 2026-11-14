#ifndef _NUVIX_VIRTIO_H
#define _NUVIX_VIRTIO_H

#include <nuvix/device.h>

#define VIRTIO_ID_BLOCK 2
#define VIRTIO_ID_RNG   4
#define VIRTIO_MAX_VQS  4

struct virtqueue;

struct virtio_dev {
    struct device dev;
    volatile void *base;
    size_t config_size;
    unsigned int irq;
    uint32_t device_id;
    uint64_t features;
    struct virtqueue *vqs[VIRTIO_MAX_VQS];
    unsigned int nvqs;
};

struct virtio_driver {
    struct driver drv;
    uint32_t device_id;
    uint64_t required_features;
    uint64_t features;
    void (*config_changed)(struct virtio_dev *dev);
};

static inline struct virtio_dev *to_virtio_device(struct device *dev)
{
    return container_of(dev, struct virtio_dev, dev);
}

static inline const struct virtio_dev *
to_virtio_device_const(const struct device *dev)
{
    return container_of(dev, struct virtio_dev, dev);
}

static inline const struct virtio_driver *
to_virtio_driver(const struct driver *drv)
{
    return container_of(drv, struct virtio_driver, drv);
}

extern const struct bus virtio_bus;

int virtio_dev_ready(struct virtio_dev *dev);

/* Configuration offsets are 32-bit aligned. */
int virtio_config_read32(struct virtio_dev *dev, unsigned int offset,
                         uint32_t *value);
int virtio_config_read64(struct virtio_dev *dev, unsigned int offset,
                         uint64_t *value);
int virtio_config_write32(struct virtio_dev *dev, unsigned int offset,
                          uint32_t value);

#endif /* _NUVIX_VIRTIO_H */
