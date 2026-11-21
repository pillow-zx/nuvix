#define pr_fmt(fmt) "virtinput: " fmt

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

#define INPUT_DEV          MKDEV(242, 0)
#define VIRTINPUT_EVENTS   64

#define NUVIX_INPUT_EV_KEY 1
#define NUVIX_INPUT_EV_REL 2

struct input_event {
    uint16_t type;
    uint16_t code;
    int32_t value;
};

struct virtinput {
    struct virtqueue *eventq;
    struct input_event *buffers;
    dma_t buffers_dma;
};

static struct input_stream {
    struct chardev chardev;
    struct wait_queue wait;
    struct input_event events[VIRTINPUT_EVENTS];
    unsigned int head;
    unsigned int tail;
    unsigned int count;
    bool overflow;
    bool opened;
} input = {.wait = WAIT_QUEUE_INITIALIZER(input.wait)};

static void virtinput_queue(struct virtinput *dev, struct input_event *event)
{
    unsigned int index = event - dev->buffers;
    struct vq_sg sg = {
        .addr = dev->buffers_dma + index * sizeof(*event),
        .len = sizeof(*event),
    };
    int ret = vq_add(dev->eventq, &sg, 0, 1, event);

    Assert(!ret, "event buffer lacked virtqueue capacity (%d)\n", ret);
}

static void virtinput_done(struct virtqueue *vq)
{
    struct virtinput *dev = vq->dev->dev.drvdata;
    struct input_event *event;
    unsigned long flags;
    uint32_t len;

    spin_lock_irqsave(&input.wait.lock, flags);
    while ((event = vq_get_buf(vq, &len))) {
        Assert(len == sizeof(*event), "invalid event length %u\n", len);
        if (input.opened && !input.overflow &&
            (event->type == NUVIX_INPUT_EV_KEY ||
             event->type == NUVIX_INPUT_EV_REL)) {
            if (input.count == VIRTINPUT_EVENTS) {
                input.head = input.tail = input.count = 0;
                input.overflow = true;
            } else {
                input.events[input.head] = *event;
                input.head = (input.head + 1) % VIRTINPUT_EVENTS;
                ++input.count;
            }
        }
        virtinput_queue(dev, event);
    }
    if (input.count || input.overflow)
        wake_up_all_locked(&input.wait);
    spin_unlock_irqrestore(&input.wait.lock, flags);
    vq_kick(vq);
}

static int virtinput_open(struct file *file)
{
    struct input_stream *stream = file->f_private;
    unsigned long flags;
    int ret = 0;

    if ((file->f_flags & O_ACCMODE) != O_RDONLY)
        return -EINVAL;
    spin_lock_irqsave(&stream->wait.lock, flags);
    if (stream->opened)
        ret = -EBUSY;
    else
        stream->opened = true;
    spin_unlock_irqrestore(&stream->wait.lock, flags);
    return ret;
}

static ssize_t virtinput_read(struct kiocb *iocb, struct kiov *iov)
{
    struct input_stream *stream = iocb->file->f_private;
    struct input_event events[VIRTINPUT_EVENTS];
    unsigned long flags;
    size_t count, bytes;
    int ret;

    if (iov->count < sizeof(events[0]) || iov->count % sizeof(events[0]))
        return -EINVAL;
    for (;;) {
        spin_lock_irqsave(&stream->wait.lock, flags);
        if (stream->overflow) {
            stream->overflow = false;
            spin_unlock_irqrestore(&stream->wait.lock, flags);
            return -EOVERFLOW;
        }
        if (stream->count)
            break;
        spin_unlock_irqrestore(&stream->wait.lock, flags);
        if (iocb->file->f_flags & O_NONBLOCK)
            return -EAGAIN;
        ret = wait_event_interruptible(&stream->wait,
                                       stream->count || stream->overflow);
        if (ret)
            return ret;
    }

    count = MIN(iov->count / sizeof(events[0]), (size_t)stream->count);
    for (size_t i = 0; i < count; ++i) {
        events[i] = stream->events[stream->tail];
        stream->tail = (stream->tail + 1) % VIRTINPUT_EVENTS;
    }
    stream->count -= count;
    spin_unlock_irqrestore(&stream->wait.lock, flags);

    bytes = count * sizeof(events[0]);
    return copy_to_kiov(events, bytes, iov) == bytes ? (ssize_t)bytes : -EFAULT;
}

static poll_t virtinput_poll(struct file *file, struct poll_table *table)
{
    struct input_stream *stream = file->f_private;
    unsigned long flags;
    poll_t mask = 0;

    spin_lock_irqsave(&stream->wait.lock, flags);
    poll_add(&stream->wait, table);
    if (stream->count)
        mask |= POLLIN;
    if (stream->overflow)
        mask |= POLLERR;
    spin_unlock_irqrestore(&stream->wait.lock, flags);
    return mask;
}

static int virtinput_release(struct file *file)
{
    struct input_stream *stream = file->f_private;
    unsigned long flags;

    spin_lock_irqsave(&stream->wait.lock, flags);
    stream->head = stream->tail = stream->count = 0;
    stream->overflow = false;
    stream->opened = false;
    spin_unlock_irqrestore(&stream->wait.lock, flags);
    return 0;
}

static const struct file_operations virtinput_fops = {
    .open = virtinput_open,
    .read = virtinput_read,
    .poll = virtinput_poll,
    .release = virtinput_release,
};

static int virtinput_probe(struct device *device)
{
    static bool registed;
    struct virtio_dev *dev = to_virtio_device(device);
    struct virtinput *source __free(kfree) = NULL;
    int ret;

    source = kzalloc(sizeof(*source));
    if (!source)
        return -ENOMEM;
    ret = vq_create(dev, VIRTINPUT_EVENTS, virtinput_done);
    if (ret)
        return ret;
    ret = vq_create(dev, VIRTINPUT_EVENTS, NULL);
    if (ret)
        return ret;
    source->buffers = dma_alloc_coherent(0, &source->buffers_dma);
    if (!source->buffers)
        return -ENOMEM;
    source->eventq = dev->vqs[0];
    device->drvdata = source;
    for (unsigned int i = 0; i < VIRTINPUT_EVENTS; ++i)
        virtinput_queue(source, &source->buffers[i]);
    ret = virtio_dev_ready(dev);
    if (ret) {
        device->drvdata = NULL;
        dma_free_coherent(source->buffers, 0);
        return ret;
    }
    vq_kick(source->eventq);

    if (!registed) {
        input.chardev = CHARDEV_INIT(INPUT_DEV, &virtinput_fops, &input);
        chardev_register(&input.chardev);
        registed = true;
        pr_info("input stream: events=%u, dev=%u:%u\n", VIRTINPUT_EVENTS,
                MAJOR(INPUT_DEV), MINOR(INPUT_DEV));
    }
    keep(source);
    return 0;
}

static struct virtio_driver virtinput_driver = {
    .drv =
        {
            .name = "virtio-input",
            .bus = &virtio_bus,
            .probe = virtinput_probe,
        },
    .device_id = VIRTIO_ID_INPUT,
};

MODULE_DRIVER_INIT(virtinput, 84);
