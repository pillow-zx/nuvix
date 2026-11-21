#define pr_fmt(fmt) "virtgpu: " fmt

#include <nuvix/assert.h>
#include <nuvix/chardev.h>
#include <nuvix/completion.h>
#include <nuvix/dma.h>
#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/layout.h>
#include <nuvix/math.h>
#include <nuvix/mm.h>
#include <nuvix/module.h>
#include <nuvix/mutex.h>
#include <nuvix/pgtable.h>
#include <nuvix/printk.h>
#include <nuvix/slab.h>
#include <nuvix/string.h>
#include <nuvix/tick.h>
#include <nuvix/tlb.h>
#include <nuvix/uaccess.h>
#include <nuvix/virtio.h>
#include <nuvix/virtqueue.h>

#include "virtgpu.h"

#define GPU_DEV             MKDEV(241, 0)
#define GPU_INFO            0x80504700U
#define GPU_PRESENT         0x00004702U

#define VIRTGPU_QUEUE_SIZE  16
#define VIRTGPU_TIMEOUT_NS  (30ULL * NSEC_PER_SEC)
#define VIRTGPU_BUFFERS     6U
#define VIRTGPU_PIXEL_BYTES 4

#define VIRTGPU_RESP_BYTES  512
#define VIRTGPU_REQ_BYTES   (PAGE_SIZE - VIRTGPU_RESP_BYTES)
#define VIRTGPU_MAX_ENTRIES                                                    \
    ((VIRTGPU_REQ_BYTES - sizeof(struct virtgpu_attach)) /                     \
     sizeof(struct virtgpu_entry))

struct virtgpu_info {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t length;
    uint64_t offsets[VIRTGPU_BUFFERS];
    uint32_t index;
    uint32_t reserved;
};

struct virtgpu_chunk {
    void *cpu;
    dma_t dma;
};

struct virtgpu_buf {
    uint32_t id;
    struct virtgpu_chunk *chunks;
};

struct virtgpu_wire {
    uint8_t request[VIRTGPU_REQ_BYTES];
    union {
        struct virtgpu_hdr hdr;
        struct virtgpu_display_info display;
    } response;
};

struct virtgpu {
    struct virtqueue *ctrlq;
    struct chardev chardev;
    struct mutex lock;
    struct completion done;
    struct virtgpu_wire *wire;
    dma_t wire_dma;
    uint32_t written;

    struct virtgpu_info info;
    struct virtgpu_buf buffers[VIRTGPU_BUFFERS];
    unsigned int nr_chunks;
    unsigned int chunk_order;
    uint32_t scanout;
    bool opened;
    bool failed;
};

static void virtgpu_done(struct virtqueue *vq)
{
    struct virtgpu *gpu;
    uint32_t written;

    if ((gpu = vq_get_buf(vq, &written))) {
        gpu->written = written;
        complete(&gpu->done);
    }
}

static int virtgpu_response(struct virtgpu *gpu, uint32_t written,
                            uint32_t expected, size_t response_size, void *out)
{
    const struct virtgpu_hdr *hdr = &gpu->wire->response.hdr;

    if (written < sizeof(*hdr) || written > response_size ||
        !(hdr->flags & VIRTIO_GPU_FLAG_FENCE) ||
        (hdr->type == expected && written != response_size)) {
        gpu->failed = true;
        return -EPROTO;
    }
    if (hdr->type == expected) {
        if (out)
            memcpy(out, &gpu->wire->response, response_size);
        return 0;
    }
    switch (hdr->type) {
    case VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY:
        return -ENOMEM;
    case VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID:
    case VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID:
    case VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER:
        return -EINVAL;
    default:
        return -EIO;
    }
}

static int virtgpu_queue_cmd(struct virtgpu *gpu, uint32_t type, void *req,
                             size_t reqsz, size_t respsz, uint64_t *deadline)
{
    struct vq_sg sg[] = {
        {gpu->wire_dma, (uint32_t)reqsz},
        {gpu->wire_dma + offsetof(struct virtgpu_wire, response),
         (uint32_t)respsz},
    };
    int ret;

    Assert(reqsz <= sizeof(gpu->wire->request),
           "GPU command does not fit in the wire buffer\n");
    ret = time_deadline_from_ns(VIRTGPU_TIMEOUT_NS, deadline);
    if (ret)
        return ret;
    ((struct virtgpu_hdr *)req)->type = type;
    ((struct virtgpu_hdr *)req)->flags = VIRTIO_GPU_FLAG_FENCE;
    memcpy(gpu->wire->request, req, reqsz);
    ret = vq_add(gpu->ctrlq, sg, 1, 1, gpu);
    if (ret)
        return ret;
    vq_kick(gpu->ctrlq);
    return 0;
}

static int virtgpu_timeout(struct virtgpu *gpu, uint32_t type)
{
    gpu->failed = true;
    pr_err("control command %#x timed out\n", type);
    return -ETIMEDOUT;
}

static int virtgpu_cmd(struct virtgpu *gpu, uint32_t type, void *req,
                       size_t reqsz)
{
    size_t respsz = sizeof(struct virtgpu_hdr);
    uint64_t deadline;
    int ret;

    reinit_completion(&gpu->done);
    ret = virtgpu_queue_cmd(gpu, type, req, reqsz, respsz, &deadline);
    if (ret)
        return ret;
    ret = wait_for_completion_until(&gpu->done, deadline);
    if (ret)
        return virtgpu_timeout(gpu, type);
    return virtgpu_response(gpu, gpu->written, VIRTIO_GPU_RESP_OK_NODATA,
                            respsz, NULL);
}

static int virtgpu_submit_frame(struct virtgpu *gpu, unsigned int index)
{
    const struct virtgpu_rect rect = {
        .width = gpu->info.width,
        .height = gpu->info.height,
    };
    uint32_t id = gpu->buffers[index].id;
    struct virtgpu_transfer transfer = {.rect = rect, .resource_id = id};
    struct virtgpu_scanout scanout = {
        .rect = rect,
        .scanout_id = gpu->scanout,
        .resource_id = id,
    };
    struct virtgpu_flush flush = {.rect = rect, .resource_id = id};
    int ret;

    ret = virtgpu_cmd(gpu, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D, &transfer,
                      sizeof(transfer));
    if (ret)
        return ret;
    ret =
        virtgpu_cmd(gpu, VIRTIO_GPU_CMD_SET_SCANOUT, &scanout, sizeof(scanout));
    if (ret)
        return ret;
    gpu->info.index = (index + 1) % VIRTGPU_BUFFERS;
    return virtgpu_cmd(gpu, VIRTIO_GPU_CMD_RESOURCE_FLUSH, &flush,
                       sizeof(flush));
}

static int virtgpu_init_cmd(struct virtgpu *gpu, uint32_t type, void *req,
                            size_t reqsz, struct virtgpu_display_info *display)
{
    size_t respsz = display ? sizeof(*display) : sizeof(struct virtgpu_hdr);
    uint32_t expected =
        display ? VIRTIO_GPU_RESP_OK_DISPLAY_INFO : VIRTIO_GPU_RESP_OK_NODATA;
    uint64_t deadline;
    uint32_t written;
    int ret;

    ret = virtgpu_queue_cmd(gpu, type, req, reqsz, respsz, &deadline);
    if (ret)
        return ret;

    while (!vq_get_buf(gpu->ctrlq, &written))
        if (time_read() >= deadline)
            return virtgpu_timeout(gpu, type);
    return virtgpu_response(gpu, written, expected, respsz, display);
}

static void virtgpu_backing_free(struct virtgpu *gpu, struct virtgpu_buf *buf)
{
    for (unsigned int i = 0; i < gpu->nr_chunks; ++i)
        dma_free_coherent(buf->chunks[i].cpu, gpu->chunk_order);
    kfree(buf->chunks);
}

static int virtgpu_backing_alloc(struct virtgpu *gpu, struct virtgpu_buf *buf)
{
    buf->chunks = kzalloc(gpu->nr_chunks * sizeof(*buf->chunks));
    if (!buf->chunks)
        return -ENOMEM;
    for (unsigned int i = 0; i < gpu->nr_chunks; ++i) {
        struct virtgpu_chunk *chunk = &buf->chunks[i];

        chunk->cpu = dma_alloc_coherent(gpu->chunk_order, &chunk->dma);
        if (!chunk->cpu) {
            virtgpu_backing_free(gpu, buf);
            return -ENOMEM;
        }
    }
    return 0;
}

static int virtgpu_create_buffer(struct virtgpu *gpu, struct virtgpu_buf *buf)
{
    size_t chunk_bytes = (size_t)PAGE_SIZE << gpu->chunk_order;
    size_t reqsz = sizeof(struct virtgpu_attach) +
                   gpu->nr_chunks * sizeof(struct virtgpu_entry);
    struct virtgpu_attach *attach __free(kfree) = kzalloc(reqsz);
    struct virtgpu_create create = {
        .resource_id = buf->id,
        .format = gpu->info.format,
        .width = gpu->info.width,
        .height = gpu->info.height,
    };
    int ret;

    if (!attach)
        return -ENOMEM;
    ret = virtgpu_init_cmd(gpu, VIRTIO_GPU_CMD_RESOURCE_CREATE_2D, &create,
                           sizeof(create), NULL);
    if (ret)
        return ret;
    attach->resource_id = buf->id;
    attach->nr_entries = gpu->nr_chunks;
    for (unsigned int i = 0; i < gpu->nr_chunks; ++i) {
        attach->entries[i].addr = buf->chunks[i].dma;
        attach->entries[i].length = (uint32_t)chunk_bytes;
    }
    return virtgpu_init_cmd(gpu, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING, attach,
                            reqsz, NULL);
}

static int virtgpu_init_display(struct virtgpu *gpu, unsigned int scanouts)
{
    struct virtgpu_info *info = &gpu->info;
    struct virtgpu_display_info display;
    struct virtgpu_hdr hdr = {0};
    uint64_t buffer_size;
    size_t chunk_bytes;
    unsigned int id, order, i;
    int ret;

    ret = virtgpu_init_cmd(gpu, VIRTIO_GPU_CMD_GET_DISPLAY_INFO, &hdr,
                           sizeof(hdr), &display);
    if (ret)
        return ret;
    for (id = 0; id < scanouts; ++id)
        if (display.modes[id].enabled)
            break;
    if (id == scanouts)
        return -ENODEV;

    gpu->scanout = id;
    info->width = display.modes[id].rect.width;
    info->height = display.modes[id].rect.height;
    info->format = VIRTIO_GPU_FORMAT_BGRX8888;
    if (!info->width || !info->height ||
        check_mul_overflow(info->width, VIRTGPU_PIXEL_BYTES, &info->stride))
        return -EIO;
    info->length = (uint64_t)info->stride * info->height;
    if (info->length > UINT32_MAX)
        return -E2BIG;
    buffer_size = ALIGN_UP(info->length, (uint64_t)PAGE_SIZE);

    for (order = 0; order < CONFIG_MAX_ORDER; ++order) {
        chunk_bytes = (size_t)PAGE_SIZE << order;
        if (DIV_ROUND_UP(info->length, chunk_bytes) <= VIRTGPU_MAX_ENTRIES)
            break;
    }
    if (order == CONFIG_MAX_ORDER)
        return -E2BIG;
    gpu->chunk_order = order;
    gpu->nr_chunks = DIV_ROUND_UP(info->length, chunk_bytes);

    for (i = 0; i < VIRTGPU_BUFFERS; ++i) {
        info->offsets[i] = i * buffer_size;
        gpu->buffers[i].id = i + 1;
        ret = virtgpu_backing_alloc(gpu, &gpu->buffers[i]);
        if (ret) {
            while (i)
                virtgpu_backing_free(gpu, &gpu->buffers[--i]);
            return ret;
        }
    }
    for (i = 0; i < VIRTGPU_BUFFERS; ++i) {
        ret = virtgpu_create_buffer(gpu, &gpu->buffers[i]);
        if (ret)
            return ret;
    }
    return 0;
}

static int virtgpu_open(struct file *file)
{
    struct virtgpu *gpu = file->f_private;
    int ret = 0;

    if ((file->f_flags & O_ACCMODE) != O_RDWR)
        return -EACCES;
    mutex_lock(&gpu->lock);
    if (gpu->failed)
        ret = -ENODEV;
    else if (gpu->opened)
        ret = -EBUSY;
    else
        gpu->opened = true;
    mutex_unlock(&gpu->lock);
    return ret;
}

static int virtgpu_release(struct file *file)
{
    struct virtgpu *gpu = file->f_private;

    mutex_lock(&gpu->lock);
    gpu->opened = false;
    mutex_unlock(&gpu->lock);
    return 0;
}

static int virtgpu_mmap(struct file *file, struct vm_struct *vma)
{
    struct virtgpu *gpu = file->f_private;
    size_t chunk_bytes = (size_t)PAGE_SIZE << gpu->chunk_order;
    size_t buffer_size = ALIGN_UP(gpu->info.length, (uint64_t)PAGE_SIZE);
    size_t size = vma->vm_end - vma->vm_start;
    vaddr_t addr = vma->vm_start;
    unsigned int index;
    int ret;

    if (!(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC) ||
        size > buffer_size)
        return -EINVAL;
    for (index = 0; index < VIRTGPU_BUFFERS; ++index)
        if (vma->vm_offset == gpu->info.offsets[index])
            break;
    if (index == VIRTGPU_BUFFERS)
        return -EINVAL;

    for (unsigned int i = 0; size; ++i) {
        size_t length = MIN(chunk_bytes, size);
        paddr_t phys = __pa((vaddr_t)gpu->buffers[index].chunks[i].cpu);

        ret = pgtable_map_io(vma->vm_mm, addr, phys, length, vma->vm_flags);
        if (ret) {
            for (vaddr_t mapped = vma->vm_start; mapped < addr;
                 mapped += PAGE_SIZE)
                pgtable_unmap_page(vma->vm_mm, mapped);
            flush_tlb_all();
            return ret;
        }
        addr += length;
        size -= length;
    }
    return 0;
}

static long virtgpu_ioctl(struct file *file, unsigned int cmd,
                          unsigned long arg)
{
    struct virtgpu *gpu = file->f_private;

    switch (cmd) {
    case GPU_INFO: {
        struct virtgpu_info info;

        mutex_lock(&gpu->lock);
        info = gpu->info;
        mutex_unlock(&gpu->lock);
        return copy_to_user((void *)arg, &info, sizeof(info)) ? -EFAULT : 0;
    }
    case GPU_PRESENT: {
        int ret;

        if (arg >= VIRTGPU_BUFFERS)
            return -EINVAL;
        mutex_lock(&gpu->lock);
        ret = gpu->failed ? -ENODEV : virtgpu_submit_frame(gpu, arg);
        mutex_unlock(&gpu->lock);
        return ret;
    }
    default:
        return -ENOTTY;
    }
}

static const struct file_operations virtgpu_fops = {
    .open = virtgpu_open,
    .release = virtgpu_release,
    .ioctl = virtgpu_ioctl,
    .mmap = virtgpu_mmap,
};

static int virtgpu_probe(struct device *device)
{
    struct virtio_dev *dev = to_virtio_device(device);
    struct virtgpu *gpu __free(kfree) = NULL;
    uint32_t scanouts;
    int ret;

    gpu = kzalloc(sizeof(*gpu));
    if (!gpu)
        return -ENOMEM;
    ret = virtio_config_read32(dev, VIRTIO_GPU_CONFIG_SCANOUTS, &scanouts);
    if (ret)
        return ret;
    if (!scanouts || scanouts > VIRTIO_GPU_MAX_SCANOUTS)
        return -EIO;
    ret = vq_create(dev, VIRTGPU_QUEUE_SIZE, virtgpu_done);
    if (ret)
        return ret;
    ret = vq_create(dev, VIRTGPU_QUEUE_SIZE, NULL);
    if (ret)
        return ret;
    gpu->wire = dma_alloc_coherent(0, &gpu->wire_dma);
    if (!gpu->wire)
        return -ENOMEM;
    gpu->ctrlq = dev->vqs[0];
    mutex_init(&gpu->lock);
    init_completion(&gpu->done);
    ret = virtio_dev_ready(dev);
    if (ret) {
        dma_free_coherent(gpu->wire, 0);
        return ret;
    }

    ret = virtgpu_init_display(gpu, scanouts);
    Assert(!ret, "display initialization failed (%d)\n", ret);
    device->drvdata = gpu;
    gpu->chardev = CHARDEV_INIT(GPU_DEV, &virtgpu_fops, gpu);
    chardev_register(&gpu->chardev);
    pr_info("device: %ux%u, stride=%u, %u %llu-byte buffers, dev=%u:%u\n",
            gpu->info.width, gpu->info.height, gpu->info.stride,
            VIRTGPU_BUFFERS, (unsigned long long)gpu->info.length,
            MAJOR(GPU_DEV), MINOR(GPU_DEV));
    keep(gpu);
    return 0;
}

static struct virtio_driver virtgpu_driver = {
    .drv =
        {
            .name = "virtio-gpu",
            .bus = &virtio_bus,
            .probe = virtgpu_probe,
        },
    .device_id = VIRTIO_ID_GPU,
};

MODULE_DRIVER_INIT(virtgpu, 84);
