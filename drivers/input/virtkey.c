#define pr_fmt(fmt) "virtkey: " fmt

#include <nuvix/assert.h>
#include <nuvix/chardev.h>
#include <nuvix/dma.h>
#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/math.h>
#include <nuvix/module.h>
#include <nuvix/printk.h>
#include <nuvix/slab.h>
#include <nuvix/virtio.h>
#include <nuvix/virtqueue.h>
#include <nuvix/wait.h>

#define KEY_DEV         MKDEV(242, 0)
#define VIRTKEY_EVENTS  64
#define VIRTKEY_EV_KEY  1
#define VIRTKEY_PRESSED 1

struct virtkey_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
};

struct virtkey {
    struct chardev chardev;
    struct virtqueue *eventq;

    struct virtkey_event *buffers;
    dma_t buffers_dma;

    struct virtkey_event events[VIRTKEY_EVENTS];
    unsigned int head;
    unsigned int tail;
    unsigned int count;

    struct wait_queue wait;
    bool overflow;
    bool opened;
};

static void virtkey_queue(struct virtkey *key, struct virtkey_event *event)
{
    unsigned int index = event - key->buffers;
    struct vq_sg sg = {
        .addr = key->buffers_dma + index * sizeof(*event),
        .len = sizeof(*event),
    };
    int ret = vq_add(key->eventq, &sg, 0, 1, event);

    Assert(!ret, "event buffer lacked virtqueue capacity (%d)\n", ret);
}

static void virtkey_done(struct virtqueue *vq)
{
    struct virtkey *key = vq->dev->dev.drvdata;
    struct virtkey_event *event;
    unsigned long flags;
    uint32_t len;

    spin_lock_irqsave(&key->wait.lock, flags);
    while ((event = vq_get_buf(vq, &len))) {
        Assert(len == sizeof(*event), "invalid event length %u\n", len);
        if (key->opened && !key->overflow && event->type == VIRTKEY_EV_KEY &&
            event->value <= VIRTKEY_PRESSED) {
            if (key->count == VIRTKEY_EVENTS) {
                key->head = key->tail = key->count = 0;
                key->overflow = true;
            } else {
                key->events[key->head] = *event;
                key->head = (key->head + 1) % VIRTKEY_EVENTS;
                ++key->count;
            }
        }
        virtkey_queue(key, event);
    }
    if (key->count || key->overflow)
        wake_up_all_locked(&key->wait);
    spin_unlock_irqrestore(&key->wait.lock, flags);
    vq_kick(vq);
}

static int virtkey_open(struct file *file)
{
    struct virtkey *key = file->f_private;
    unsigned long flags;
    int ret = 0;

    if ((file->f_flags & O_ACCMODE) != O_RDONLY)
        return -EINVAL;
    spin_lock_irqsave(&key->wait.lock, flags);
    if (key->opened)
        ret = -EBUSY;
    else
        key->opened = true;
    spin_unlock_irqrestore(&key->wait.lock, flags);
    return ret;
}

static ssize_t virtkey_read(struct kiocb *iocb, struct kiov *iov)
{
    struct virtkey *key = iocb->file->f_private;
    struct virtkey_event events[VIRTKEY_EVENTS];
    unsigned long flags;
    size_t count, bytes;
    int ret;

    if (iov->count < sizeof(events[0]) || iov->count % sizeof(events[0]))
        return -EINVAL;
    for (;;) {
        spin_lock_irqsave(&key->wait.lock, flags);
        if (key->overflow) {
            key->overflow = false;
            spin_unlock_irqrestore(&key->wait.lock, flags);
            return -EOVERFLOW;
        }
        if (key->count)
            break;
        spin_unlock_irqrestore(&key->wait.lock, flags);
        if (iocb->file->f_flags & O_NONBLOCK)
            return -EAGAIN;
        ret = wait_event_interruptible(&key->wait, key->count || key->overflow);
        if (ret)
            return ret;
    }

    count = MIN(iov->count / sizeof(events[0]), (size_t)key->count);
    for (size_t i = 0; i < count; ++i) {
        events[i] = key->events[key->tail];
        key->tail = (key->tail + 1) % VIRTKEY_EVENTS;
    }
    key->count -= count;
    spin_unlock_irqrestore(&key->wait.lock, flags);

    bytes = count * sizeof(events[0]);
    return copy_to_kiov(events, bytes, iov) == bytes ? (ssize_t)bytes : -EFAULT;
}

static int virtkey_release(struct file *file)
{
    struct virtkey *key = file->f_private;
    unsigned long flags;

    spin_lock_irqsave(&key->wait.lock, flags);
    key->head = key->tail = key->count = 0;
    key->overflow = false;
    key->opened = false;
    spin_unlock_irqrestore(&key->wait.lock, flags);
    return 0;
}

static const struct file_operations virtkey_fops = {
    .open = virtkey_open,
    .read = virtkey_read,
    .release = virtkey_release,
};

static int virtkey_probe(struct device *device)
{
    struct virtio_dev *dev = to_virtio_device(device);
    struct virtkey *key __free(kfree) = NULL;
    int ret;

    key = kzalloc(sizeof(*key));
    if (!key)
        return -ENOMEM;
    ret = vq_create(dev, VIRTKEY_EVENTS, virtkey_done);
    if (ret)
        return ret;
    ret = vq_create(dev, VIRTKEY_EVENTS, NULL);
    if (ret)
        return ret;
    key->buffers = dma_alloc_coherent(0, &key->buffers_dma);
    if (!key->buffers)
        return -ENOMEM;
    key->eventq = dev->vqs[0];
    wait_queue_init(&key->wait);
    device->drvdata = key;
    for (unsigned int i = 0; i < VIRTKEY_EVENTS; ++i)
        virtkey_queue(key, &key->buffers[i]);
    ret = virtio_dev_ready(dev);
    if (ret) {
        device->drvdata = NULL;
        dma_free_coherent(key->buffers, 0);
        return ret;
    }
    vq_kick(key->eventq);
    key->chardev = CHARDEV_INIT(KEY_DEV, &virtkey_fops, key);
    chardev_register(&key->chardev);
    pr_info("device: events=%u, dev=%u:%u\n", VIRTKEY_EVENTS, MAJOR(KEY_DEV),
            MINOR(KEY_DEV));
    keep(key);
    return 0;
}

static struct virtio_driver virtkey_driver = {
    .drv =
        {
            .name = "virtio-keyboard",
            .bus = &virtio_bus,
            .probe = virtkey_probe,
        },
    .device_id = VIRTIO_ID_INPUT,
};

MODULE_DRIVER_INIT(virtkey, 85);
