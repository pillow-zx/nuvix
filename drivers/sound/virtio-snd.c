#define pr_fmt(fmt) "virtio-snd: " fmt

#include <nuvix/chardev.h>
#include <nuvix/dma.h>
#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/math.h>
#include <nuvix/macro.h>
#include <nuvix/module.h>
#include <nuvix/page.h>
#include <nuvix/printk.h>
#include <nuvix/slab.h>
#include <nuvix/sleep.h>
#include <nuvix/tick.h>
#include <nuvix/uaccess.h>
#include <nuvix/virtio.h>
#include <nuvix/virtqueue.h>

#define SOUND_DEV    MKDEV(240, 0)
#define AUDIO_CONFIG 0x40084e00U
#define AUDIO_DRAIN  0x00004e01U

struct audio_config {
    unsigned int rate;
    unsigned int channels;
};

#define VIRTIO_SND_CONFIG_STREAMS 4
#define VIRTIO_SND_VQ_CONTROL     0
#define VIRTIO_SND_VQ_TX          2
#define VIRTIO_SND_VQ_COUNT       4
#define VIRTIO_SND_QUEUE_SIZE     32
#define VIRTIO_SND_TIMEOUT_NS     (30ULL * NSEC_PER_SEC)
#define VIRTIO_SND_BUFFER_ORDER   3
#define VIRTIO_SND_BUFFERS        (1U << VIRTIO_SND_BUFFER_ORDER)

#define VIRTIO_SND_R_PCM_INFO     0x0100
#define VIRTIO_SND_R_SET_PARAMS   0x0101
#define VIRTIO_SND_R_PREPARE      0x0102
#define VIRTIO_SND_R_RELEASE      0x0103
#define VIRTIO_SND_R_START        0x0104
#define VIRTIO_SND_R_STOP         0x0105
#define VIRTIO_SND_FMT_S16        5
#define VIRTIO_SND_S_OK           0x8000
#define VIRTIO_SND_S_BAD_MSG      0x8001
#define VIRTIO_SND_S_NOT_SUPP     0x8002
#define VIRTIO_SND_D_OUTPUT       0
#define VIRTIO_SND_D_INPUT        1

struct virtio_snd_query {
    uint32_t code;
    uint32_t start_id;
    uint32_t count;
    uint32_t size;
};

struct virtio_snd_pcm_info {
    uint8_t reserved[8];
    uint64_t formats;
    uint64_t rates;
    uint8_t direction;
    uint8_t channels_min;
    uint8_t channels_max;
    uint8_t padding[5];
};

struct virtio_snd_command {
    uint32_t code;
    uint32_t stream_id;
};

struct virtio_snd_params {
    struct virtio_snd_command hdr;
    uint32_t buffer_bytes;
    uint32_t period_bytes;
    uint8_t reserved[4];
    uint8_t channels;
    uint8_t format;
    uint8_t rate;
    uint8_t padding[1];
};

struct virtio_snd_tx {
    uint32_t stream_id;
    struct {
        uint32_t code;
        uint32_t latency_bytes;
    } status;
};

struct virtio_snd_wire {
    union {
        struct virtio_snd_query query;
        struct virtio_snd_command command;
        struct virtio_snd_params params;
    } request;
    uint32_t status;
    struct virtio_snd_pcm_info info;
    struct virtio_snd_tx tx[VIRTIO_SND_BUFFERS];
};

struct virtio_snd {
    struct virtio_dev *dev;
    struct chardev chardev;
    struct mutex lock;
    bool open;
    bool configured;
    bool running;
    unsigned int frame_bytes;
    unsigned int bytes_per_second;
    uint32_t latency_bytes;
    void *samples;
    dma_t samples_dma;
    struct wait_queue available;
    unsigned int pending;
    int tx_error;
    uint32_t streams;
    uint32_t stream_id;
    struct virtio_snd_wire *wire;
    dma_t wire_dma;
};

static int virtio_snd_control(struct virtio_snd *snd, unsigned int request_size,
                              unsigned int response_size)
{
    struct virtqueue *vq = snd->dev->vqs[VIRTIO_SND_VQ_CONTROL];
    struct vq_sg sg[] = {
        {snd->wire_dma + offsetof(struct virtio_snd_wire, request),
         request_size},
        {snd->wire_dma + offsetof(struct virtio_snd_wire, status),
         sizeof(snd->wire->status)},
        {snd->wire_dma + offsetof(struct virtio_snd_wire, info), response_size},
    };
    uint64_t deadline;
    uint32_t written = 0;
    int ret;

    ret = time_deadline_from_ns(VIRTIO_SND_TIMEOUT_NS, &deadline);
    if (ret)
        return ret;
    snd->wire->status = UINT32_MAX;
    ret = vq_add(vq, sg, 1, response_size ? 2 : 1, snd);
    Assert(!ret, "sound control request lacked queue capacity (%d)\n", ret);
    vq_kick(vq);
    while (!vq_get_buf(vq, &written))
        Assert(time_read() < deadline, "sound control request timed out\n");

    if (written < sizeof(snd->wire->status))
        return -EIO;
    switch (snd->wire->status) {
    case VIRTIO_SND_S_OK:
        return written == sizeof(snd->wire->status) + response_size ? 0 : -EIO;
    case VIRTIO_SND_S_BAD_MSG:
        return -EINVAL;
    case VIRTIO_SND_S_NOT_SUPP:
        return -EOPNOTSUPP;
    default:
        return -EIO;
    }
}

static int virtio_snd_query_pcm(struct virtio_snd *snd, uint32_t id)
{
    snd->wire->request.query = (struct virtio_snd_query){
        .code = VIRTIO_SND_R_PCM_INFO,
        .start_id = id,
        .count = 1,
        .size = sizeof(snd->wire->info),
    };
    return virtio_snd_control(snd, sizeof(snd->wire->request.query),
                              sizeof(snd->wire->info));
}

static int virtio_snd_command(struct virtio_snd *snd, uint32_t code)
{
    snd->wire->request.command =
        (struct virtio_snd_command){code, snd->stream_id};
    return virtio_snd_control(snd, sizeof(snd->wire->request.command), 0);
}

static int virtio_snd_find_playback(struct virtio_snd *snd)
{
    for (uint32_t id = 0; id < snd->streams; ++id) {
        struct virtio_snd_pcm_info *info = &snd->wire->info;
        int ret = virtio_snd_query_pcm(snd, id);

        if (ret)
            return ret;
        if (info->direction > VIRTIO_SND_D_INPUT || !info->channels_min ||
            info->channels_min > info->channels_max)
            return -EIO;
        if (info->direction != VIRTIO_SND_D_OUTPUT)
            continue;
        snd->stream_id = id;
        pr_info(
            "playback: stream=%u, channels=%u..%u, formats=%llx, rates=%llx\n",
            id, info->channels_min, info->channels_max,
            (unsigned long long)info->formats, (unsigned long long)info->rates);
        return 0;
    }
    return -ENODEV;
}

static int virtio_snd_configure(struct virtio_snd *snd,
                                unsigned int sample_rate, unsigned int channels)
{
    static const unsigned int rates[] = {
        5512,  8000,  11025, 16000, 22050,  32000,  44100,
        48000, 64000, 88200, 96000, 176400, 192000, 384000,
    };
    const struct virtio_snd_pcm_info *info = &snd->wire->info;
    unsigned int frame_bytes = channels * sizeof(int16_t);
    unsigned int period_bytes;
    int rate;
    int ret;

    if (channels < info->channels_min || channels > info->channels_max ||
        !(info->formats & (1ULL << VIRTIO_SND_FMT_S16)))
        return -EINVAL;
    for (rate = 0; rate < ARRLEN(rates); ++rate)
        if (rates[rate] == sample_rate)
            break;
    if (rate == ARRLEN(rates) || !(info->rates & (1ULL << rate)))
        return -EINVAL;

    period_bytes = PAGE_SIZE - PAGE_SIZE % frame_bytes;
    snd->wire->request.params = (struct virtio_snd_params){
        .hdr = {VIRTIO_SND_R_SET_PARAMS, snd->stream_id},
        .buffer_bytes = period_bytes * VIRTIO_SND_BUFFERS,
        .period_bytes = period_bytes,
        .channels = channels,
        .format = VIRTIO_SND_FMT_S16,
        .rate = rate,
    };
    ret = virtio_snd_control(snd, sizeof(snd->wire->request.params), 0);
    if (ret)
        return ret;
    ret = virtio_snd_command(snd, VIRTIO_SND_R_PREPARE);
    if (!ret) {
        snd->frame_bytes = frame_bytes;
        snd->bytes_per_second = sample_rate * frame_bytes;
        snd->latency_bytes = 0;
        snd->configured = true;
    }
    return ret;
}

static int virtio_snd_reserve(struct virtio_snd *snd)
{
    if (snd->tx_error)
        return snd->tx_error;
    for (unsigned int i = 0; i < VIRTIO_SND_BUFFERS; ++i) {
        if (!(snd->pending & (1U << i))) {
            snd->pending |= 1U << i;
            return (int)i;
        }
    }
    return -EAGAIN;
}

static void virtio_snd_return_buffer(struct virtio_snd *snd, unsigned int slot,
                                     int error)
{
    unsigned long flags;

    spin_lock_irqsave(&snd->available.lock, flags);
    if (error)
        snd->tx_error = error;
    snd->pending &= ~(1U << slot);
    wake_up_all_locked(&snd->available);
    spin_unlock_irqrestore(&snd->available.lock, flags);
}

static void virtio_snd_tx_done(struct virtqueue *vq)
{
    struct virtio_snd *snd = vq->dev->dev.drvdata;
    struct virtio_snd_tx *tx;
    uint32_t written;

    while ((tx = vq_get_buf(vq, &written))) {
        int error =
            written == sizeof(tx->status) && tx->status.code == VIRTIO_SND_S_OK
                ? 0
                : -EIO;

        snd->latency_bytes = tx->status.latency_bytes;
        virtio_snd_return_buffer(snd, tx - snd->wire->tx, error);
    }
}

static int virtio_snd_submit(struct virtio_snd *snd, unsigned int slot,
                             size_t count)
{
    struct virtqueue *vq = snd->dev->vqs[VIRTIO_SND_VQ_TX];
    struct virtio_snd_tx *tx;
    dma_t tx_dma;
    int ret;

    if (!snd->running) {
        ret = virtio_snd_command(snd, VIRTIO_SND_R_START);
        if (ret) {
            virtio_snd_return_buffer(snd, slot, 0);
            return ret;
        }
        snd->running = true;
    }
    tx = &snd->wire->tx[slot];
    tx_dma = snd->wire_dma + offsetof(struct virtio_snd_wire, tx) +
             slot * sizeof(*tx);
    tx->stream_id = snd->stream_id;
    tx->status.code = UINT32_MAX;
    struct vq_sg sg[] = {
        {tx_dma, sizeof(tx->stream_id)},
        {snd->samples_dma + slot * PAGE_SIZE, count},
        {tx_dma + offsetof(struct virtio_snd_tx, status), sizeof(tx->status)},
    };

    ret = vq_add(vq, sg, 2, 1, tx);
    Assert(!ret, "sound buffer lacked queue capacity (%d)\n", ret);
    vq_kick(vq);
    return 0;
}

static int virtio_snd_stop(struct virtio_snd *snd)
{
    int ret;

    if (snd->running) {
        ret = virtio_snd_command(snd, VIRTIO_SND_R_STOP);
        if (ret)
            return ret;
    }
    ret = virtio_snd_command(snd, VIRTIO_SND_R_RELEASE);
    if (ret)
        return ret;
    wait_event(&snd->available, !snd->pending);
    snd->configured = false;
    snd->running = false;
    return 0;
}

static int virtio_snd_drain(struct virtio_snd *snd)
{
    int error;
    int ret;

    ret = wait_event_interruptible(&snd->available,
                                   (error = snd->tx_error) || !snd->pending);
    if (ret)
        return ret;
    if (error)
        return error;
    ret = sleep_ns(DIV_ROUND_UP((uint64_t)snd->latency_bytes * NSEC_PER_SEC,
                                snd->bytes_per_second));
    if (ret)
        return ret;
    return virtio_snd_stop(snd);
}

static int sound_open(struct file *file)
{
    struct virtio_snd *snd = file->f_private;
    int ret = 0;

    if ((file->f_flags & O_ACCMODE) != O_WRONLY)
        return -EINVAL;
    if (file->f_flags & O_NONBLOCK)
        return -EOPNOTSUPP;
    mutex_lock(&snd->lock);
    if (snd->open) {
        ret = -EBUSY;
    } else {
        ret = 0;
        if (snd->stream_id == UINT32_MAX)
            ret = virtio_snd_find_playback(snd);
        if (!ret) {
            snd->tx_error = 0;
            snd->open = true;
        }
    }
    mutex_unlock(&snd->lock);
    return ret;
}

static ssize_t sound_write(struct kiocb *iocb, struct kiov *iov)
{
    struct virtio_snd *snd = iocb->file->f_private;
    size_t done = 0, limit;
    int ret;

    ret = mutex_lock_interruptible(&snd->lock);
    if (ret)
        return ret;
    if (!snd->configured || iov->count % snd->frame_bytes) {
        ret = -EINVAL;
        goto out;
    }
    limit = PAGE_SIZE - PAGE_SIZE % snd->frame_bytes;
    while (iov->count) {
        struct kiov next = *iov;
        size_t count = MIN(iov->count, limit);
        int slot;

        ret = wait_event_interruptible(
            &snd->available, (slot = virtio_snd_reserve(snd)) != -EAGAIN);
        if (ret)
            break;
        if (slot < 0) {
            ret = slot;
            break;
        }
        if (copy_from_kiov((char *)snd->samples + slot * PAGE_SIZE, count,
                           &next) != count) {
            virtio_snd_return_buffer(snd, slot, 0);
            ret = -EFAULT;
            break;
        }
        ret = virtio_snd_submit(snd, slot, count);
        if (ret)
            break;
        *iov = next;
        done += count;
    }
out:
    mutex_unlock(&snd->lock);
    return done ? (ssize_t)done : ret;
}

static long sound_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct virtio_snd *snd = file->f_private;
    struct audio_config config;
    int ret;

    if (cmd != AUDIO_CONFIG && cmd != AUDIO_DRAIN)
        return -ENOTTY;
    ret = mutex_lock_interruptible(&snd->lock);
    if (ret)
        return ret;
    if (cmd == AUDIO_CONFIG) {
        if (!snd->open || snd->configured)
            ret = -EBUSY;
        else if (copy_from_user(&config, (const void *)arg, sizeof(config)))
            ret = -EFAULT;
        else
            ret = virtio_snd_configure(snd, config.rate, config.channels);
    } else if (snd->configured) {
        ret = virtio_snd_drain(snd);
    }
    mutex_unlock(&snd->lock);
    return ret;
}

static int sound_release(struct file *file)
{
    struct virtio_snd *snd = file->f_private;
    int ret = 0;

    mutex_lock(&snd->lock);
    if (snd->configured)
        ret = virtio_snd_stop(snd);
    if (!ret)
        snd->open = false;
    mutex_unlock(&snd->lock);
    return ret;
}

static const struct file_operations sound_fops = {
    .open = sound_open,
    .write = sound_write,
    .ioctl = sound_ioctl,
    .release = sound_release,
};

static int virtio_snd_probe(struct device *device)
{
    struct virtio_dev *dev = to_virtio_device(device);
    struct virtio_snd *snd __free(kfree) = NULL;
    void (*callbacks[])(struct virtqueue *) = {
        NULL,
        NULL,
        virtio_snd_tx_done,
        NULL,
    };
    int ret;

    snd = kzalloc(sizeof(*snd));
    if (!snd)
        return -ENOMEM;
    ret = virtio_config_read32(dev, VIRTIO_SND_CONFIG_STREAMS, &snd->streams);
    if (ret)
        return ret;
    if (!snd->streams)
        return -ENODEV;
    for (unsigned int i = 0; i < VIRTIO_SND_VQ_COUNT; ++i) {
        ret = vq_create(dev, VIRTIO_SND_QUEUE_SIZE, callbacks[i]);
        if (ret)
            return ret;
    }
    snd->wire = dma_alloc_coherent(0, &snd->wire_dma);
    if (!snd->wire)
        return -ENOMEM;
    snd->samples =
        dma_alloc_coherent(VIRTIO_SND_BUFFER_ORDER, &snd->samples_dma);
    if (!snd->samples) {
        dma_free_coherent(snd->wire, 0);
        return -ENOMEM;
    }
    snd->dev = dev;
    snd->stream_id = UINT32_MAX;
    mutex_init(&snd->lock);
    wait_queue_init(&snd->available);
    device->drvdata = snd;
    ret = virtio_dev_ready(dev);
    if (ret) {
        device->drvdata = NULL;
        dma_free_coherent(snd->samples, VIRTIO_SND_BUFFER_ORDER);
        dma_free_coherent(snd->wire, 0);
        return ret;
    }
    snd->chardev = (struct chardev){
        .dev = SOUND_DEV,
        .fops = &sound_fops,
        .private = snd,
    };
    chardev_register(&snd->chardev);
    pr_info("device: streams=%u\n", snd->streams);
    keep(snd);
    return 0;
}

static struct virtio_driver virtio_snd_driver = {
    .drv =
        {
            .name = "virtio-snd",
            .bus = &virtio_bus,
            .probe = virtio_snd_probe,
        },
    .device_id = VIRTIO_ID_SOUND,
};

MODULE_DRIVER_INIT(virtio_snd, 83);
