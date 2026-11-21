#define pr_fmt(fmt) "virtsnd: " fmt

#include <nuvix/assert.h>
#include <nuvix/bitmap.h>
#include <nuvix/bitops.h>
#include <nuvix/chardev.h>
#include <nuvix/completion.h>
#include <nuvix/dma.h>
#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/module.h>
#include <nuvix/mutex.h>
#include <nuvix/page.h>
#include <nuvix/printk.h>
#include <nuvix/slab.h>
#include <nuvix/tick.h>
#include <nuvix/uaccess.h>
#include <nuvix/virtio.h>
#include <nuvix/virtqueue.h>
#include <nuvix/wait.h>

#include "virtsnd.h"

#define SND_DEV           MKDEV(240, 0)
#define AUDIO_INFO        0x80104e00U

#define SND_RATE          44100U
#define SND_CHANNELS      2U
#define SND_PERIODS       4U
#define SND_PERIOD_FRAMES 512U
#define SND_PERIOD_BYTES  (SND_PERIOD_FRAMES * SND_CHANNELS * sizeof(int16_t))
#define SND_BUFFER_BYTES  (SND_PERIODS * SND_PERIOD_BYTES)
#define SND_BUFFER_ORDER  1
#define SND_PENDING_MASK  GENMASK(SND_PERIODS - 1, 0)

#define SND_QSZ           32
#define SND_TIMEOUT_NS    (30ULL * NSEC_PER_SEC)

struct virtsnd_info {
    uint32_t rate;
    uint32_t channels;
    uint32_t bits;
    uint32_t period_bytes;
};

struct virtsnd_wire {
    union {
        struct virtsnd_query query;
        struct virtsnd_cmd cmd;
        struct virtsnd_params params;
    } req;
    uint32_t status;
    struct virtsnd_pcm_info info;
    uint32_t stream_id;
    struct virtsnd_pcm_status pcm[SND_PERIODS];
};

struct virtsnd {
    struct virtio_dev *dev;
    struct virtqueue *txq;
    struct chardev chardev;
    struct virtsnd_wire *wire;
    dma_t wire_dma;
    char *buffer;
    dma_t buffer_dma;

    struct mutex lock;
    bool opened;
    bool started;
    int broken;

    struct completion control_done;
    uint32_t control_written;

    struct wait_queue wait;
    struct completion tx_idle;
    unsigned long pending;
    int error;
};

static int virtsnd_break(struct virtsnd *snd, int error)
{
    unsigned long flags;

    snd->broken = error;
    spin_lock_irqsave(&snd->wait.lock, flags);
    snd->error = error;
    wake_up_all_locked(&snd->wait);
    spin_unlock_irqrestore(&snd->wait.lock, flags);
    return error;
}

static int virtsnd_err(uint32_t code)
{
    switch (code) {
    case VIRTIO_SND_S_OK:
        return 0;
    case VIRTIO_SND_S_BAD_MSG:
        return -EINVAL;
    case VIRTIO_SND_S_NOT_SUPP:
        return -EOPNOTSUPP;
    default:
        return -EIO;
    }
}

static void virtsnd_control_done(struct virtqueue *vq)
{
    struct virtsnd *snd;
    uint32_t len;

    if ((snd = vq_get_buf(vq, &len))) {
        snd->control_written = len;
        complete(&snd->control_done);
    }
}

static int virtsnd_ctrl(struct virtsnd *snd, unsigned int reqsz,
                        unsigned int respsz)
{
    struct virtqueue *vq = snd->dev->vqs[VIRTIO_SND_VQ_CONTROL];
    const size_t status_size = sizeof(snd->wire->status);
    struct vq_sg sg[] = {
        {snd->wire_dma + offsetof(struct virtsnd_wire, req), reqsz},
        {snd->wire_dma + offsetof(struct virtsnd_wire, status), status_size},
        {snd->wire_dma + offsetof(struct virtsnd_wire, info), respsz},
    };
    uint64_t deadline;
    uint32_t len;
    int ret;

    ret = time_deadline_from_ns(SND_TIMEOUT_NS, &deadline);
    if (ret)
        return ret;

    snd->wire->status = UINT32_MAX;
    reinit_completion(&snd->control_done);
    ret = vq_add(vq, sg, 1, respsz ? 2 : 1, snd);
    if (ret)
        return ret;
    vq_kick(vq);

    ret = wait_for_completion_until(&snd->control_done, deadline);
    if (ret) {
        pr_err("control request failed while waiting for completion (%d)\n",
               ret);
        return virtsnd_break(snd, ret);
    }

    len = snd->control_written;
    if (len < status_size || len > status_size + respsz)
        return virtsnd_break(snd, -EPROTO);
    ret = virtsnd_err(snd->wire->status);
    if (ret)
        return ret;
    if (len != status_size + respsz)
        return virtsnd_break(snd, -EPROTO);
    return 0;
}

static int virtsnd_cmd(struct virtsnd *snd, uint32_t code)
{
    snd->wire->req.cmd = (struct virtsnd_cmd){code, snd->wire->stream_id};
    return virtsnd_ctrl(snd, sizeof(snd->wire->req.cmd), 0);
}

static int virtsnd_find(struct virtsnd *snd)
{
    uint32_t streams;
    int ret;

    ret = virtio_config_read32(snd->dev, VIRTIO_SND_CONFIG_STREAMS, &streams);
    if (ret)
        return ret;

    for (uint32_t id = 0; id < streams; ++id) {
        snd->wire->req.query = (struct virtsnd_query){
            .code = VIRTIO_SND_R_PCM_INFO,
            .start_id = id,
            .count = 1,
            .size = sizeof(snd->wire->info),
        };
        ret = virtsnd_ctrl(snd, sizeof(snd->wire->req.query),
                           sizeof(snd->wire->info));
        if (ret)
            return ret;
        if (snd->wire->info.direction > VIRTIO_SND_D_INPUT)
            return -EPROTO;
        if (snd->wire->info.direction == VIRTIO_SND_D_OUTPUT) {
            snd->wire->stream_id = id;
            return 0;
        }
    }
    return -ENODEV;
}

static int virtsnd_params(struct virtsnd *snd)
{
    snd->wire->req.params = (struct virtsnd_params){
        .hdr = {VIRTIO_SND_R_SET_PARAMS, snd->wire->stream_id},
        .buffer_bytes = SND_BUFFER_BYTES,
        .period_bytes = SND_PERIOD_BYTES,
        .channels = SND_CHANNELS,
        .format = VIRTIO_SND_FMT_S16,
        .rate = VIRTIO_SND_RATE_44100,
    };
    return virtsnd_ctrl(snd, sizeof(snd->wire->req.params), 0);
}

static void virtsnd_tx_done(struct virtqueue *vq)
{
    struct virtsnd *snd = vq->dev->dev.drvdata;
    struct virtsnd_pcm_status *pcm;
    unsigned long flags, pending;
    uint32_t len;

    spin_lock_irqsave(&snd->wait.lock, flags);
    pending = snd->pending;
    while ((pcm = vq_get_buf(vq, &len))) {
        if (!snd->error) {
            if (len != sizeof(*pcm))
                snd->error = -EPROTO;
            else if (pcm->code != VIRTIO_SND_S_OK)
                snd->error = -EIO;
        }
        snd->pending &= ~BIT(pcm - snd->wire->pcm);
    }
    if (snd->pending != pending) {
        if (!snd->pending)
            complete(&snd->tx_idle);
        wake_up_all_locked(&snd->wait);
    }
    spin_unlock_irqrestore(&snd->wait.lock, flags);
}

static int virtsnd_queue_period(struct virtsnd *snd, unsigned long index)
{
    struct virtsnd_pcm_status *pcm = &snd->wire->pcm[index];
    struct vq_sg sg[] = {
        {snd->wire_dma + offsetof(struct virtsnd_wire, stream_id),
         sizeof(snd->wire->stream_id)},
        {snd->buffer_dma + index * SND_PERIOD_BYTES, SND_PERIOD_BYTES},
        {snd->wire_dma + offsetof(struct virtsnd_wire, pcm) +
             index * sizeof(*pcm),
         sizeof(*pcm)},
    };
    unsigned long flags;
    int ret;

    spin_lock_irqsave(&snd->wait.lock, flags);
    ret = snd->error;
    if (ret)
        goto out;

    pcm->code = UINT32_MAX;
    ret = vq_add(snd->txq, sg, 2, 1, pcm);
    if (ret)
        goto out;
    if (!snd->pending)
        reinit_completion(&snd->tx_idle);
    snd->pending |= BIT(index);
out:
    spin_unlock_irqrestore(&snd->wait.lock, flags);
    if (!ret)
        vq_kick(snd->txq);
    return ret;
}

static ssize_t virtsnd_write(struct kiocb *iocb, struct kiov *iov)
{
    struct file *file = iocb->file;
    struct virtsnd *snd = file->f_private;
    struct kiov next = *iov;
    unsigned long flags, index;
    int ret;

    if (iov->count != SND_PERIOD_BYTES)
        return -EINVAL;

    for (;;) {
        ret = mutex_lock_interruptible(&snd->lock);
        if (ret)
            return ret;

        spin_lock_irqsave(&snd->wait.lock, flags);
        ret = snd->error;
        index = find_first_zero_bit(&snd->pending, SND_PERIODS);
        spin_unlock_irqrestore(&snd->wait.lock, flags);

        if (ret || index < SND_PERIODS)
            break;
        mutex_unlock(&snd->lock);
        if (READ_ONCE(file->f_flags) & O_NONBLOCK)
            return -EAGAIN;

        ret = wait_event_interruptible(
            &snd->wait, snd->error || snd->pending != SND_PENDING_MASK);
        if (ret)
            return ret;
    }

    if (ret)
        goto out;
    if (copy_from_kiov(snd->buffer + index * SND_PERIOD_BYTES, SND_PERIOD_BYTES,
                       &next) != SND_PERIOD_BYTES) {
        ret = -EFAULT;
        goto out;
    }

    if (!snd->started) {
        ret = virtsnd_cmd(snd, VIRTIO_SND_R_START);
        if (ret) {
            if (!snd->broken) {
                spin_lock_irqsave(&snd->wait.lock, flags);
                snd->error = ret;
                wake_up_all_locked(&snd->wait);
                spin_unlock_irqrestore(&snd->wait.lock, flags);
            }
            goto out;
        }
        snd->started = true;
    }

    ret = virtsnd_queue_period(snd, index);
    if (!ret)
        *iov = next;
out:
    mutex_unlock(&snd->lock);
    return ret ? ret : (ssize_t)SND_PERIOD_BYTES;
}

static poll_t virtsnd_poll(struct file *file, struct poll_table *table)
{
    struct virtsnd *snd = file->f_private;
    unsigned long flags;
    poll_t mask = 0;

    spin_lock_irqsave(&snd->wait.lock, flags);
    poll_add(&snd->wait, table);
    if (snd->error)
        mask = POLLERR;
    else if (snd->pending != SND_PENDING_MASK)
        mask = POLLOUT;
    spin_unlock_irqrestore(&snd->wait.lock, flags);

    return mask;
}

static int virtsnd_open(struct file *file)
{
    struct virtsnd *snd = file->f_private;
    unsigned long flags;
    int ret;

    if (!FILE_WRITE(file))
        return -EACCES;

    mutex_lock(&snd->lock);
    if (snd->opened) {
        ret = -EBUSY;
        goto out;
    }
    if (snd->broken) {
        ret = snd->broken;
        goto out;
    }

    ret = virtsnd_find(snd);
    if (!ret)
        ret = virtsnd_params(snd);
    if (!ret)
        ret = virtsnd_cmd(snd, VIRTIO_SND_R_PREPARE);
    if (ret)
        goto out;

    spin_lock_irqsave(&snd->wait.lock, flags);
    snd->error = 0;
    spin_unlock_irqrestore(&snd->wait.lock, flags);
    snd->opened = true;
out:
    mutex_unlock(&snd->lock);
    return ret;
}

static int virtsnd_wait_idle(struct virtsnd *snd)
{
    unsigned long flags;
    uint64_t deadline;
    bool idle;
    int ret;

    spin_lock_irqsave(&snd->wait.lock, flags);
    idle = !snd->pending;
    spin_unlock_irqrestore(&snd->wait.lock, flags);
    if (idle)
        return 0;

    ret = time_deadline_from_ns(SND_TIMEOUT_NS, &deadline);
    if (ret)
        return ret;
    return wait_for_completion_until(&snd->tx_idle, deadline);
}

static int virtsnd_release(struct file *file)
{
    struct virtsnd *snd = file->f_private;
    unsigned long flags;
    int ret;

    mutex_lock(&snd->lock);
    ret = snd->broken;
    if (!ret && snd->started)
        ret = virtsnd_cmd(snd, VIRTIO_SND_R_STOP);
    if (!ret)
        ret = virtsnd_cmd(snd, VIRTIO_SND_R_RELEASE);
    if (!ret)
        ret = virtsnd_wait_idle(snd);

    if (ret) {
        virtsnd_break(snd, ret);
    } else {
        spin_lock_irqsave(&snd->wait.lock, flags);
        ret = snd->error;
        spin_unlock_irqrestore(&snd->wait.lock, flags);
    }
    snd->started = false;
    snd->opened = false;
    mutex_unlock(&snd->lock);
    return ret;
}

static long virtsnd_ioctl(struct file *file, unsigned int cmd,
                          unsigned long arg)
{
    static const struct virtsnd_info info = {
        .rate = SND_RATE,
        .channels = SND_CHANNELS,
        .bits = 16,
        .period_bytes = SND_PERIOD_BYTES,
    };

    (void)file;
    if (cmd != AUDIO_INFO)
        return -ENOTTY;
    return copy_to_user((void *)arg, &info, sizeof(info)) ? -EFAULT : 0;
}

static const struct file_operations virtsnd_fops = {
    .open = virtsnd_open,
    .write = virtsnd_write,
    .poll = virtsnd_poll,
    .ioctl = virtsnd_ioctl,
    .release = virtsnd_release,
};

static int virtsnd_probe(struct device *device)
{
    struct virtio_dev *dev = to_virtio_device(device);
    struct virtsnd *snd __free(kfree) = NULL;
    void (*callbacks[VIRTIO_SND_VQ_COUNT])(struct virtqueue *) = {
        [VIRTIO_SND_VQ_CONTROL] = virtsnd_control_done,
        [VIRTIO_SND_VQ_TX] = virtsnd_tx_done,
    };
    int ret;

    snd = kzalloc(sizeof(*snd));
    if (!snd)
        return -ENOMEM;
    for (unsigned int i = 0; i < VIRTIO_SND_VQ_COUNT; ++i) {
        ret = vq_create(dev, SND_QSZ, callbacks[i]);
        if (ret)
            return ret;
    }

    snd->wire = dma_alloc_coherent(0, &snd->wire_dma);
    if (!snd->wire)
        return -ENOMEM;
    snd->buffer = dma_alloc_coherent(SND_BUFFER_ORDER, &snd->buffer_dma);
    if (!snd->buffer) {
        ret = -ENOMEM;
        goto free;
    }

    snd->dev = dev;
    snd->txq = dev->vqs[VIRTIO_SND_VQ_TX];
    mutex_init(&snd->lock);
    wait_queue_init(&snd->wait);
    init_completion(&snd->control_done);
    init_completion(&snd->tx_idle);
    device->drvdata = snd;
    ret = virtio_dev_ready(dev);
    if (ret) {
        device->drvdata = NULL;
        dma_free_coherent(snd->buffer, SND_BUFFER_ORDER);
        goto free;
    }

    snd->chardev = CHARDEV_INIT(SND_DEV, &virtsnd_fops, snd);
    chardev_register(&snd->chardev);
    pr_info("device: rate=%u, ch=%u, %u periods of %zu bytes\n", SND_RATE,
            SND_CHANNELS, SND_PERIODS, SND_PERIOD_BYTES);
    keep(snd);
    return 0;

free:
    dma_free_coherent(snd->wire, 0);
    return ret;
}

static struct virtio_driver virtsnd_driver = {
    .drv =
        {
            .name = "virtio-snd",
            .bus = &virtio_bus,
            .probe = virtsnd_probe,
        },
    .device_id = VIRTIO_ID_SOUND,
};

MODULE_DRIVER_INIT(virtsnd, 83);
