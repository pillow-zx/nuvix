#include <nuvix/virtio.h>
#include <nuvix/virtqueue.h>
#include <nuvix/assert.h>
#include <nuvix/errno.h>
#include <nuvix/irq.h>
#include <nuvix/math.h>

#define VIRTIO_COMMON_FEATURES (((1ULL << 50) - 1) & ~((1ULL << 24) - 1))
#define VIRTIO_RESET_POLLS     100000

static bool virtio_match(const struct device *dev, const struct driver *drv)
{
    const struct virtio_dev *vdev = to_virtio_device_const(dev);
    const struct virtio_driver *vdrv = to_virtio_driver(drv);

    return vdev->device_id == vdrv->device_id;
}

static int virtio_reset(struct virtio_dev *dev)
{
    dev->ops->set_status(dev, 0);
    for (unsigned int i = 0; i < VIRTIO_RESET_POLLS; ++i)
        if (!dev->ops->get_status(dev))
            return 0;
    return -ETIMEDOUT;
}

static int virtio_negotiate(struct virtio_dev *dev)
{
    const struct virtio_driver *drv = to_virtio_driver(dev->dev.drv);
    uint64_t required = drv->required_features | VIRTIO_F_VERSION_1;
    uint64_t unsupported = VIRTIO_COMMON_FEATURES & ~VIRTIO_F_VERSION_1;
    uint32_t status;

    dev->device_features = dev->ops->get_features(dev);
    if ((required & unsupported) ||
        (dev->device_features & required) != required)
        return -EOPNOTSUPP;
    dev->features =
        dev->device_features & (required | drv->features) & ~unsupported;
    dev->ops->set_features(dev, dev->features);
    status = dev->ops->get_status(dev) | VIRTIO_STATUS_FEATURES_OK;
    dev->ops->set_status(dev, status);
    status = dev->ops->get_status(dev);
    if (!(status & VIRTIO_STATUS_FEATURES_OK) ||
        (status & (VIRTIO_STATUS_NEEDS_RESET | VIRTIO_STATUS_FAILED)))
        return -EIO;
    return 0;
}

static void virtio_irq(void *data)
{
    struct virtio_dev *dev = data;
    const struct virtio_driver *drv = to_virtio_driver(dev->dev.drv);
    uint32_t events = dev->ops->ack_irq(dev);

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

static void virtio_abort(struct virtio_dev *dev)
{
    uint32_t status = dev->ops->get_status(dev);

    Assert(!(status & VIRTIO_STATUS_DRIVER_OK),
           "virtio probe failed after device start\n");
    if (dev->irq_registered) {
        Assert(!irq_unregister(dev->irq), "virtio IRQ unregister failed\n");
        dev->irq_registered = false;
    }
    dev->ops->set_status(dev, status | VIRTIO_STATUS_FAILED);
    Assert(!virtio_reset(dev), "virtio reset did not complete\n");
    for (unsigned int i = 0; i < dev->nvqs; ++i) {
        virtqueue_destroy(dev->vqs[i]);
        dev->vqs[i] = NULL;
    }
    dev->nvqs = 0;
    dev->features = 0;
}

static int virtio_probe(struct device *device)
{
    struct virtio_dev *dev = to_virtio_device(device);
    const struct virtio_driver *drv = to_virtio_driver(device->drv);
    int ret;

    Assert(drv->nvqs <= VIRTIO_MAX_VQS, "too many virtio queues\n");
    ret = virtio_reset(dev);
    if (ret)
        goto fail;
    dev->ops->set_status(dev, VIRTIO_STATUS_ACK);
    dev->ops->set_status(dev, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    ret = virtio_negotiate(dev);
    if (ret)
        goto fail;
    for (unsigned int i = 0; i < drv->nvqs; ++i) {
        unsigned int num = drv->vq_size[i];
        struct virtqueue *vq;

        Assert(IS_POWER_OF_2(num) && num <= VIRTQUEUE_MAX_SIZE,
               "invalid virtio queue size %u\n", num);
        if (num > dev->ops->queue_max(dev, i)) {
            ret = -ENOSPC;
            goto fail;
        }
        vq = virtqueue_create(dev, i, num, drv->vq_cb[i]);
        if (IS_ERR(vq)) {
            ret = PTR_ERR(vq);
            goto fail;
        }
        dev->vqs[dev->nvqs++] = vq;
        ret = dev->ops->setup_vq(dev, vq);
        if (ret)
            goto fail;
    }
    ret = irq_register(dev->irq, virtio_irq, dev);
    if (ret)
        goto fail;
    dev->irq_registered = true;
    ret = device->drv->probe(device);
    if (!ret)
        return 0;
fail:
    virtio_abort(dev);
    return ret;
}

const struct bus virtio_bus = {
    .name = "virtio",
    .match = virtio_match,
    .probe = virtio_probe,
};

int virtio_dev_ready(struct virtio_dev *dev)
{
    uint32_t status = dev->ops->get_status(dev);
    int ret;

    if (status & VIRTIO_STATUS_DRIVER_OK)
        return 0;
    if (status & (VIRTIO_STATUS_NEEDS_RESET | VIRTIO_STATUS_FAILED))
        return -EIO;
    ret = irq_enable(dev->irq);
    if (ret)
        return ret;
    dev->ops->set_status(dev, status | VIRTIO_STATUS_DRIVER_OK);
    return 0;
}

int virtio_config_read(struct virtio_dev *dev, unsigned int offset, void *buf,
                       size_t length)
{
    return dev->ops->config_read(dev, offset, buf, length);
}

int virtio_config_read32(struct virtio_dev *dev, unsigned int offset,
                         uint32_t *value)
{
    uint32_t result;
    int ret = virtio_config_read(dev, offset, &result, sizeof(result));

    if (!ret)
        *value = result;
    return ret;
}

int virtio_config_read64(struct virtio_dev *dev, unsigned int offset,
                         uint64_t *value)
{
    uint64_t result;
    int ret = virtio_config_read(dev, offset, &result, sizeof(result));

    if (!ret)
        *value = result;
    return ret;
}
