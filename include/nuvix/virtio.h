#ifndef _NUVIX_VIRTIO_H
#define _NUVIX_VIRTIO_H

#include <nuvix/device.h>

#define VIRTIO_ID_BLOCK           2
#define VIRTIO_ID_RNG             4
#define VIRTIO_STATUS_ACK         1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_NEEDS_RESET 64
#define VIRTIO_STATUS_FAILED      128
#define VIRTIO_F_VERSION_1        (1ULL << 32)
#define VIRTIO_IRQ_QUEUE          1
#define VIRTIO_IRQ_CONFIG         2
#define VIRTIO_MAX_VQS            4

struct dtb;
struct virtio_dev;
struct virtio_driver;
struct virtqueue;

struct virtio_transport_ops {
    uint32_t (*get_status)(struct virtio_dev *dev);
    void (*set_status)(struct virtio_dev *dev, uint32_t status);
    uint64_t (*get_features)(struct virtio_dev *dev);
    void (*set_features)(struct virtio_dev *dev, uint64_t features);
    int (*config_read)(struct virtio_dev *dev, unsigned int offset, void *buf,
                       size_t length);
    unsigned int (*queue_max)(struct virtio_dev *dev, unsigned int index);
    int (*setup_vq)(struct virtio_dev *dev, struct virtqueue *vq);
    void (*notify)(struct virtio_dev *dev, struct virtqueue *vq);
    uint32_t (*ack_irq)(struct virtio_dev *dev);
};

struct virtio_dev {
    struct device dev;
    const struct virtio_transport_ops *ops;
    unsigned int irq;
    uint32_t device_id;
    uint32_t vendor_id;
    uint64_t device_features;
    uint64_t features;
    struct virtqueue *vqs[VIRTIO_MAX_VQS];
    unsigned int nvqs;
    bool irq_registered;
};

struct virtio_driver {
    struct driver drv;
    uint32_t device_id;
    uint64_t required_features;
    uint64_t features;
    unsigned int nvqs;
    uint16_t vq_size[VIRTIO_MAX_VQS];
    void (*vq_cb[VIRTIO_MAX_VQS])(struct virtqueue *vq);
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

int virtio_mmio_scan(const struct dtb *dtb);
int virtio_blk_register(void);
int virtio_rng_register(void);

int virtio_dev_ready(struct virtio_dev *dev);

int virtio_config_read(struct virtio_dev *dev, unsigned int offset, void *buf,
                       size_t length);
int virtio_config_read32(struct virtio_dev *dev, unsigned int offset,
                         uint32_t *value);
int virtio_config_read64(struct virtio_dev *dev, unsigned int offset,
                         uint64_t *value);

#endif /* _NUVIX_VIRTIO_H */
