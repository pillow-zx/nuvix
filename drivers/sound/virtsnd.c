#define pr_fmt(fmt) "virtsnd: " fmt

#include <nuvix/assert.h>
#include <nuvix/chardev.h>
#include <nuvix/completion.h>
#include <nuvix/dma.h>
#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/math.h>
#include <nuvix/mm.h>
#include <nuvix/module.h>
#include <nuvix/mutex.h>
#include <nuvix/page.h>
#include <nuvix/pgtable.h>
#include <nuvix/printk.h>
#include <nuvix/slab.h>
#include <nuvix/sleep.h>
#include <nuvix/tick.h>
#include <nuvix/uaccess.h>
#include <nuvix/virtio.h>
#include <nuvix/virtqueue.h>

#include "virtsnd.h"

#define SND_DEV         MKDEV(240, 0)
#define SND_RATE        44100U
#define SND_CH          1U
#define SND_FRAME       (SND_CH * sizeof(int16_t))
#define SND_QSZ         32
#define SND_TIMEOUT     (30ULL * NSEC_PER_SEC)

#define AUDIO_INFO      0x80104e00U
#define AUDIO_PLAY      0x00004e02U

#define VIRTSND_PERIODS 4
#define VIRTSND_PERIOD  (PAGE_SIZE / VIRTSND_PERIODS)

struct virtsnd_resource {
    uint32_t rate;
    uint32_t ch;
    uint32_t bits;
    uint32_t period;

    void *buf;
    dma_t buf_dma;
    struct completion done[VIRTSND_PERIODS];
};

struct virtsnd_wire {
    union {
        struct virtsnd_query query;
        struct virtsnd_cmd cmd;
        struct virtsnd_params params;
    } req;
    uint32_t status;
    struct virtsnd_pcm_info info;
    uint32_t id;
    struct virtsnd_pcm_status pcm[VIRTSND_PERIODS];
};

struct virtsnd {
    struct virtio_dev *dev;
    struct virtqueue *txq;
    struct chardev chardev;

    struct mutex lock;
    struct virtsnd_wire *wire;
    dma_t wire_dma;

    bool opened;
    struct virtsnd_resource res;
};

#define VIRTSND_INFO_BYTES (4 * sizeof(uint32_t))

static inline int virtsnd_err(uint32_t code)
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

static int virtsnd_ctrl(struct virtsnd *snd, unsigned int reqsz,
                        unsigned int respsz)
{
    struct virtqueue *vq = snd->dev->vqs[VIRTIO_SND_VQ_CONTROL];
    struct vq_sg sg[] = {
        {snd->wire_dma + offsetof(struct virtsnd_wire, req), reqsz},
        {snd->wire_dma + offsetof(struct virtsnd_wire, status),
         sizeof(snd->wire->status)},
        {snd->wire_dma + offsetof(struct virtsnd_wire, info), respsz},
    };
    uint64_t deadline;
    uint32_t len;
    int ret;

    ret = time_deadline_from_ns(SND_TIMEOUT, &deadline);
    if (ret)
        return ret;
    snd->wire->status = UINT32_MAX;
    ret = vq_add(vq, sg, 1, respsz ? 2 : 1, snd);
    Assert(!ret, "control queue submit failed (%d)\n", ret);
    vq_kick(vq);
    while (!vq_get_buf(vq, &len))
        Assert(time_read() < deadline, "sound control request timed out\n");
    if (len < sizeof(snd->wire->status))
        return -EIO;
    ret = virtsnd_err(snd->wire->status);
    if (ret)
        return ret;
    return len == sizeof(snd->wire->status) + respsz ? 0 : -EIO;
}

static int virtsnd_cmd(struct virtsnd *snd, uint32_t code)
{
    snd->wire->req.cmd = (struct virtsnd_cmd){code, snd->wire->id};
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
            return -EIO;
        if (snd->wire->info.direction == VIRTIO_SND_D_OUTPUT) {
            snd->wire->id = id;
            return 0;
        }
    }
    return -ENODEV;
}

static int virtsnd_params(struct virtsnd *snd)
{
    struct virtsnd_resource *res = &snd->res;

    snd->wire->req.params = (struct virtsnd_params){
        .hdr = {VIRTIO_SND_R_SET_PARAMS, snd->wire->id},
        .buffer_bytes = VIRTSND_PERIODS * res->period,
        .period_bytes = res->period,
        .channels = res->ch,
        .format = VIRTIO_SND_FMT_S16,
        .rate = VIRTIO_SND_RATE_44100,
    };
    return virtsnd_ctrl(snd, sizeof(snd->wire->req.params), 0);
}

static void virtsnd_done(struct virtqueue *vq)
{
    struct completion *done;
    uint32_t len;

    while ((done = vq_get_buf(vq, &len)))
        complete(done);
}

static int virtsnd_play(struct virtsnd *snd, unsigned long offset)
{
    struct virtsnd_resource *res = &snd->res;
    struct virtsnd_pcm_status *pcm;
    struct completion *done;
    struct vq_sg sg[3];
    uint64_t deadline;
    uint32_t index;
    int ret;

    if (offset % res->period || offset / res->period >= VIRTSND_PERIODS)
        return -EINVAL;
    index = offset / res->period;
    done = &res->done[index];
    pcm = &snd->wire->pcm[index];
    mutex_lock(&snd->lock);
    ret = time_deadline_from_ns(SND_TIMEOUT, &deadline);
    if (ret)
        goto out;
    pcm->code = UINT32_MAX;
    sg[0] = (struct vq_sg){snd->wire_dma + offsetof(struct virtsnd_wire, id),
                           sizeof(snd->wire->id)};
    sg[1] = (struct vq_sg){res->buf_dma + offset, res->period};
    sg[2] = (struct vq_sg){snd->wire_dma + offsetof(struct virtsnd_wire, pcm) +
                               index * sizeof(*pcm),
                           sizeof(*pcm)};
    ret = vq_add(snd->txq, sg, 2, 1, done);
    Assert(!ret, "transmit queue rejected period %u\n", index);
    vq_kick(snd->txq);
    /* Return ownership of the mapped period before userspace fills it again. */
    ret = wait_for_completion_until(done, deadline);
    Assert(!ret, "PCM period %u was never returned\n", index);
    ret = virtsnd_err(READ_ONCE(pcm->code));
out:
    mutex_unlock(&snd->lock);
    return ret;
}

static int virtsnd_open(struct file *file)
{
    struct virtsnd *snd = file->f_private;
    struct virtsnd_resource *res = &snd->res;
    int ret;

    if ((file->f_flags & O_ACCMODE) != O_RDWR)
        return -EINVAL;
    if (file->f_flags & O_NONBLOCK)
        return -EOPNOTSUPP;
    mutex_lock(&snd->lock);
    if (snd->opened) {
        ret = -EBUSY;
        goto out;
    }
    ret = virtsnd_find(snd);
    if (!ret)
        ret = virtsnd_params(snd);
    if (!ret)
        ret = virtsnd_cmd(snd, VIRTIO_SND_R_PREPARE);
    if (!ret)
        ret = virtsnd_cmd(snd, VIRTIO_SND_R_START);
    if (ret)
        goto out;
    for (unsigned int i = 0; i < VIRTSND_PERIODS; ++i) {
        reinit_completion(&res->done[i]);
        snd->wire->pcm[i] = (struct virtsnd_pcm_status){
            .code = VIRTIO_SND_S_OK,
        };
    }
    snd->opened = true;
out:
    mutex_unlock(&snd->lock);
    return ret;
}

static int virtsnd_release(struct file *file)
{
    struct virtsnd *snd = file->f_private;
    uint32_t latency = 0;
    int ret, err;

    for (unsigned int i = 0; i < VIRTSND_PERIODS; ++i)
        latency = MAX(latency, snd->wire->pcm[i].latency_bytes);
    ret = sleep_ns(DIV_ROUND_UP((uint64_t)latency * NSEC_PER_SEC,
                                (uint64_t)SND_RATE * SND_FRAME));
    mutex_lock(&snd->lock);
    err = virtsnd_cmd(snd, VIRTIO_SND_R_STOP);
    if (!err)
        err = virtsnd_cmd(snd, VIRTIO_SND_R_RELEASE);
    snd->opened = false;
    mutex_unlock(&snd->lock);
    return ret ? ret : err;
}

static int virtsnd_mmap(struct file *file, struct vm_struct *vma)
{
    struct virtsnd *snd = file->f_private;
    struct virtsnd_resource *res = &snd->res;
    size_t size = VIRTSND_PERIODS * res->period;
    size_t len = vma->vm_end - vma->vm_start;

    if (!(vma->vm_flags & VM_SHARED) || vma->vm_offset > size ||
        len > size - vma->vm_offset)
        return -EINVAL;
    return pgtable_map_io(vma->vm_mm, vma->vm_start,
                          arch_dma_to_phys(res->buf_dma) + vma->vm_offset, len,
                          vma->vm_flags);
}

static long virtsnd_ioctl(struct file *file, unsigned int cmd,
                          unsigned long arg)
{
    struct virtsnd *snd = file->f_private;

    switch (cmd) {
    case AUDIO_INFO:
        return copy_to_user((void *)arg, &snd->res, VIRTSND_INFO_BYTES)
                   ? -EFAULT
                   : 0;
    case AUDIO_PLAY:
        return virtsnd_play(snd, arg);
    default:
        return -ENOTTY;
    }
}

static const struct file_operations virtsnd_fops = {
    .open = virtsnd_open,
    .mmap = virtsnd_mmap,
    .ioctl = virtsnd_ioctl,
    .release = virtsnd_release,
};

static int virtsnd_probe(struct device *device)
{
    struct virtio_dev *dev = to_virtio_device(device);
    struct virtsnd *snd __free(kfree) = NULL;
    struct virtsnd_resource *res;
    void (*callbacks[])(struct virtqueue *) = {NULL, NULL, virtsnd_done, NULL};
    int ret;

    snd = kzalloc(sizeof(*snd));
    if (!snd)
        return -ENOMEM;
    res = &snd->res;
    for (unsigned int i = 0; i < VIRTIO_SND_VQ_COUNT; ++i) {
        ret = vq_create(dev, SND_QSZ, callbacks[i]);
        if (ret)
            return ret;
    }
    snd->wire = dma_alloc_coherent(0, &snd->wire_dma);
    if (!snd->wire)
        return -ENOMEM;
    res->buf = dma_alloc_coherent(0, &res->buf_dma);
    if (!res->buf) {
        dma_free_coherent(snd->wire, 0);
        return -ENOMEM;
    }
    snd->dev = dev;
    snd->txq = dev->vqs[VIRTIO_SND_VQ_TX];
    res->rate = SND_RATE;
    res->ch = SND_CH;
    res->bits = 16;
    res->period = VIRTSND_PERIOD;
    for (unsigned int i = 0; i < VIRTSND_PERIODS; ++i)
        init_completion(&res->done[i]);
    mutex_init(&snd->lock);
    device->drvdata = snd;
    ret = virtio_dev_ready(dev);
    if (ret) {
        device->drvdata = NULL;
        dma_free_coherent(res->buf, 0);
        dma_free_coherent(snd->wire, 0);
        return ret;
    }
    snd->chardev = CHARDEV_INIT(SND_DEV, &virtsnd_fops, snd);
    chardev_register(&snd->chardev);
    pr_info("device: rate=%u, ch=%u, bits=%u, period=%u\n", res->rate, res->ch,
            res->bits, res->period);
    keep(snd);
    return 0;
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
