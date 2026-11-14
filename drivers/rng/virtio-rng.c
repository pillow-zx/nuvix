#define pr_fmt(fmt) "virtio-rng: " fmt

#include <nuvix/random.h>
#include <nuvix/module.h>
#include <nuvix/virtio.h>
#include <nuvix/virtqueue.h>
#include <nuvix/assert.h>
#include <nuvix/dma.h>
#include <nuvix/errno.h>
#include <nuvix/printk.h>

#define VIRTIO_RNG_QUEUE_SIZE 1

static int virtio_rng_probe(struct device *device)
{
    struct virtio_dev *dev = to_virtio_device(device);
    struct virtqueue *vq;
    dma_t buffer_dma;
    void *buffer;
    uint32_t received = 0;
    int ret;

    ret = vq_create(dev, VIRTIO_RNG_QUEUE_SIZE, NULL);
    if (ret)
        return ret;
    vq = dev->vqs[0];
    buffer = dma_alloc_coherent(0, &buffer_dma);
    if (!buffer)
        return -ENOMEM;
    ret = virtio_dev_ready(dev);
    if (ret) {
        dma_free_coherent(buffer, 0);
        return ret;
    }
    while (received < CHACHA20_KEY_SIZE) {
        struct vq_sg sg = {
            .addr = buffer_dma + received,
            .len = CHACHA20_KEY_SIZE - received,
        };
        uint32_t written;

        ret = vq_add(vq, &sg, 0, 1, buffer);
        Assert(!ret, "entropy request lacked virtqueue capacity (%d)\n", ret);
        vq_kick(vq);
        while (!vq_get_buf(vq, &written))
            ;
        Assert(written && written <= sg.len,
               "invalid entropy response length %u\n", written);
        received += written;
    }
    random_seed(buffer);
    dma_free_coherent(buffer, 0);
    pr_info("device: queue=%u\n", VIRTIO_RNG_QUEUE_SIZE);
    return 0;
}

static struct virtio_driver virtio_rng_driver = {
    .drv =
        {
            .name = "virtio-rng",
            .bus = &virtio_bus,
            .probe = virtio_rng_probe,
        },
    .device_id = VIRTIO_ID_RNG,
};

MODULE_DRIVER_INIT(virtio_rng, 82);
