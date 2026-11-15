#define pr_fmt(fmt) "virtgpu: " fmt

#include <nuvix/assert.h>
#include <nuvix/chardev.h>
#include <nuvix/completion.h>
#include <nuvix/dma.h>
#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/irq.h>
#include <nuvix/layout.h>
#include <nuvix/math.h>
#include <nuvix/mm.h>
#include <nuvix/module.h>
#include <nuvix/mutex.h>
#include <nuvix/page.h>
#include <nuvix/pgtable.h>
#include <nuvix/printk.h>
#include <nuvix/slab.h>
#include <nuvix/string.h>
#include <nuvix/tick.h>
#include <nuvix/uaccess.h>
#include <nuvix/virtio.h>
#include <nuvix/virtqueue.h>

#include "virtgpu.h"

#define GPU_DEV             MKDEV(241, 0)
#define GPU_INFO            0x80184700U
#define GPU_PRESENT         0x00004701U

#define VIRTGPU_QUEUE_SIZE  16
#define VIRTGPU_TIMEOUT_NS  (30ULL * NSEC_PER_SEC)
#define VIRTGPU_RESOURCE_ID 1
#define VIRTGPU_PIXEL_BYTES 4

#define VIRTGPU_RESP_BYTES  512
#define VIRTGPU_REQ_BYTES   (PAGE_SIZE - VIRTGPU_RESP_BYTES)

#define VIRTGPU_MAX_ENTRIES                                                    \
    ((VIRTGPU_REQ_BYTES - sizeof(struct virtgpu_attach)) /                     \
     sizeof(struct virtgpu_entry))
#define VIRTGPU_INFO_BYTES                                                     \
    (4 * sizeof(unsigned int) + sizeof(unsigned long long))

struct virtgpu_resource {
    unsigned int width;
    unsigned int height;
    unsigned int stride;
    unsigned int format;
    unsigned long long size;

    uint32_t id;
    void **chunks;
    uint32_t nr_chunks;
    unsigned int chunk_order;
};

struct virtgpu_wire {
    union {
        struct virtgpu_hdr hdr;
        struct virtgpu_create create;
        uint8_t raw[VIRTGPU_REQ_BYTES];
    } request;
    union {
        struct virtgpu_hdr hdr;
        struct virtgpu_display_info display;
        uint8_t raw[VIRTGPU_RESP_BYTES];
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

    struct virtgpu_resource res;
};

static size_t virtgpu_resp_size(uint32_t resptype)
{
    return resptype == VIRTIO_GPU_RESP_OK_DISPLAY_INFO
               ? sizeof(struct virtgpu_display_info)
               : sizeof(struct virtgpu_hdr);
}

static int virtgpu_check(struct virtgpu *gpu, uint32_t written,
                         uint32_t resptype, void *resp)
{
    struct virtgpu_hdr *hdr = &gpu->wire->response.hdr;
    size_t respsz = virtgpu_resp_size(resptype);

    if (written < sizeof(*hdr) || !(hdr->flags & VIRTIO_GPU_FLAG_FENCE))
        return -EIO;
    if (hdr->type == resptype) {
        if (written != respsz)
            return -EIO;
        if (resp)
            memcpy(resp, &gpu->wire->response, respsz);
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

static void virtgpu_done(struct virtqueue *vq)
{
    struct virtgpu *gpu;
    uint32_t written;

    if ((gpu = vq_get_buf(vq, &written))) {
        gpu->written = written;
        complete(&gpu->done);
    }
}

static void virtgpu_submit(struct virtgpu *gpu, uint32_t reqtype,
                           const void *req, size_t reqsz, uint32_t resptype)
{
    size_t respsz = virtgpu_resp_size(resptype);
    struct vq_sg sg[] = {
        {gpu->wire_dma, (uint32_t)reqsz},
        {gpu->wire_dma + VIRTGPU_REQ_BYTES, (uint32_t)respsz},
    };
    int ret;

    Assert(reqsz <= VIRTGPU_REQ_BYTES && respsz <= VIRTGPU_RESP_BYTES,
           "control command does not fit the wire (%u/%u bytes)\n",
           (unsigned int)reqsz, (unsigned int)respsz);
    memcpy(gpu->wire, req, reqsz);
    gpu->wire->request.hdr = (struct virtgpu_hdr){
        .type = reqtype,
        .flags = VIRTIO_GPU_FLAG_FENCE,
    };
    ret = vq_add(gpu->ctrlq, sg, 1, 1, gpu);
    Assert(!ret, "control queue submit failed (%d)\n", ret);
    vq_kick(gpu->ctrlq);
}

static int virtgpu_cmd(struct virtgpu *gpu, uint32_t reqtype, const void *req,
                       size_t reqsz, uint32_t resptype, void *resp)
{
    uint64_t deadline;
    int ret;

    ret = time_deadline_from_ns(VIRTGPU_TIMEOUT_NS, &deadline);
    if (ret)
        return ret;
    mutex_lock(&gpu->lock);
    reinit_completion(&gpu->done);
    virtgpu_submit(gpu, reqtype, req, reqsz, resptype);
    ret = wait_for_completion_until(&gpu->done, deadline);
    Assert(!ret, "control command %#x timed out (%d)\n", reqtype, ret);
    ret = virtgpu_check(gpu, gpu->written, resptype, resp);
    mutex_unlock(&gpu->lock);
    return ret;
}

static int virtgpu_present(struct virtgpu *gpu, struct virtgpu_resource *res)
{
    struct virtgpu_rect rect = {
        .width = res->width,
        .height = res->height,
    };
    struct virtgpu_transfer transfer = {
        .rect = rect,
        .resource_id = res->id,
    };
    struct virtgpu_flush flush = {
        .rect = rect,
        .resource_id = res->id,
    };
    int ret;

    ret = virtgpu_cmd(gpu, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D, &transfer,
                      sizeof(transfer), VIRTIO_GPU_RESP_OK_NODATA, NULL);
    if (ret)
        return ret;
    return virtgpu_cmd(gpu, VIRTIO_GPU_CMD_RESOURCE_FLUSH, &flush,
                       sizeof(flush), VIRTIO_GPU_RESP_OK_NODATA, NULL);
}

static void virtgpu_backing_free(struct virtgpu_resource *res)
{
    for (unsigned int i = 0; i < res->nr_chunks; ++i)
        dma_free_coherent(res->chunks[i], res->chunk_order);
    kfree(res->chunks);
    res->chunks = NULL;
    res->nr_chunks = 0;
}

static int virtgpu_backing_alloc(struct virtgpu_resource *res)
{
    size_t size = res->size;
    size_t chunk_bytes;
    dma_t dma;
    unsigned int order;

    for (order = 0; order < CONFIG_MAX_ORDER; ++order) {
        chunk_bytes = (size_t)PAGE_SIZE << order;
        if ((size + chunk_bytes - 1) / chunk_bytes <= VIRTGPU_MAX_ENTRIES)
            break;
    }
    if (order == CONFIG_MAX_ORDER)
        return -E2BIG;
    res->chunk_order = order;
    res->nr_chunks = (size + chunk_bytes - 1) / chunk_bytes;
    res->chunks = kzalloc(res->nr_chunks * sizeof(*res->chunks));
    if (!res->chunks)
        return -ENOMEM;
    for (unsigned int i = 0; i < res->nr_chunks; ++i) {
        res->chunks[i] = dma_alloc_coherent(order, &dma);
        if (!res->chunks[i]) {
            virtgpu_backing_free(res);
            return -ENOMEM;
        }
    }
    return 0;
}

static int virtgpu_init_cmd(struct virtgpu *gpu, uint32_t reqtype,
                            const void *req, size_t reqsz, uint32_t resptype,
                            void *resp)
{
    uint64_t deadline;
    uint32_t written;
    int ret;

    ret = time_deadline_from_ns(VIRTGPU_TIMEOUT_NS, &deadline);
    if (ret)
        return ret;
    virtgpu_submit(gpu, reqtype, req, reqsz, resptype);
    while (!vq_get_buf(gpu->ctrlq, &written))
        Assert(time_read() < deadline, "control command %#x timed out\n",
               reqtype);
    return virtgpu_check(gpu, written, resptype, resp);
}

static int virtgpu_attach(struct virtgpu *gpu, struct virtgpu_resource *res)
{
    size_t chunk_bytes = (size_t)PAGE_SIZE << res->chunk_order;
    size_t reqsz = sizeof(struct virtgpu_attach) +
                   res->nr_chunks * sizeof(struct virtgpu_entry);
    struct virtgpu_attach *req;
    unsigned int i;
    int ret;

    req = kzalloc(reqsz);
    if (!req)
        return -ENOMEM;
    req->resource_id = res->id;
    req->nr_entries = res->nr_chunks;
    for (i = 0; i < res->nr_chunks; ++i) {
        req->entries[i].addr = arch_phys_to_dma(__pa((vaddr_t)res->chunks[i]));
        req->entries[i].length = (uint32_t)chunk_bytes;
    }
    ret = virtgpu_init_cmd(gpu, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING, req,
                           reqsz, VIRTIO_GPU_RESP_OK_NODATA, NULL);
    kfree(req);
    return ret;
}

static int virtgpu_init_display(struct virtgpu *gpu, unsigned int scanouts)
{
    struct virtgpu_resource *res = &gpu->res;
    struct virtgpu_display_info display;
    struct virtgpu_create create;
    struct virtgpu_scanout scanout;
    struct virtgpu_hdr hdr = {0};
    unsigned int id;
    int ret;

    ret = virtgpu_init_cmd(gpu, VIRTIO_GPU_CMD_GET_DISPLAY_INFO, &hdr,
                           sizeof(hdr), VIRTIO_GPU_RESP_OK_DISPLAY_INFO,
                           &display);
    if (ret)
        return ret;
    for (id = 0; id < scanouts; ++id)
        if (display.modes[id].enabled)
            break;
    if (id == scanouts)
        return -ENODEV;
    res->id = VIRTGPU_RESOURCE_ID;
    res->width = display.modes[id].rect.width;
    res->height = display.modes[id].rect.height;
    res->format = VIRTIO_GPU_FORMAT_BGRX8888;
    if (!res->width || !res->height ||
        check_mul_overflow(res->width, VIRTGPU_PIXEL_BYTES, &res->stride))
        return -EIO;
    res->size = (uint64_t)res->stride * res->height;
    if (res->size > UINT32_MAX)
        return -E2BIG;
    ret = virtgpu_backing_alloc(res);
    if (ret)
        return ret;

    create = (struct virtgpu_create){
        .resource_id = res->id,
        .format = res->format,
        .width = res->width,
        .height = res->height,
    };
    ret = virtgpu_init_cmd(gpu, VIRTIO_GPU_CMD_RESOURCE_CREATE_2D, &create,
                           sizeof(create), VIRTIO_GPU_RESP_OK_NODATA, NULL);
    if (ret)
        return ret;
    ret = virtgpu_attach(gpu, res);
    if (ret)
        return ret;
    scanout = (struct virtgpu_scanout){
        .rect = {.width = res->width, .height = res->height},
        .scanout_id = id,
        .resource_id = res->id,
    };
    return virtgpu_init_cmd(gpu, VIRTIO_GPU_CMD_SET_SCANOUT, &scanout,
                            sizeof(scanout), VIRTIO_GPU_RESP_OK_NODATA, NULL);
}

static int virtgpu_mmap(struct file *file, struct vm_struct *vma)
{
    struct virtgpu *gpu = file->f_private;
    struct virtgpu_resource *res = &gpu->res;
    size_t chunk_bytes = (size_t)PAGE_SIZE << res->chunk_order;
    size_t map_size = ALIGN_UP(res->size, (size_t)PAGE_SIZE);
    size_t size = vma->vm_end - vma->vm_start;
    uint64_t offset = vma->vm_offset;
    vaddr_t addr = vma->vm_start;
    int ret;

    if (!(vma->vm_flags & VM_SHARED) || offset > map_size ||
        size > map_size - offset)
        return -EINVAL;
    for (unsigned int i = (unsigned int)(offset / chunk_bytes); size; ++i) {
        size_t skip = offset - (size_t)i * chunk_bytes;
        size_t length = MIN(chunk_bytes - skip, size);

        ret = pgtable_map_io(vma->vm_mm, addr,
                             __pa((vaddr_t)res->chunks[i]) + skip, length,
                             vma->vm_flags);
        if (ret)
            return ret;
        addr += length;
        offset += length;
        size -= length;
    }
    return 0;
}

static long virtgpu_ioctl(struct file *file, unsigned int cmd,
                          unsigned long arg)
{
    struct virtgpu *gpu = file->f_private;

    switch (cmd) {
    case GPU_INFO:
        return copy_to_user((void *)arg, &gpu->res, VIRTGPU_INFO_BYTES)
                   ? -EFAULT
                   : 0;
    case GPU_PRESENT:
        if (!FILE_WRITE(file))
            return -EBADF;
        return virtgpu_present(gpu, &gpu->res);
    default:
        return -ENOTTY;
    }
}

static const struct file_operations virtgpu_fops = {
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
    device->drvdata = gpu;
    ret = virtio_dev_ready(dev);
    if (ret) {
        device->drvdata = NULL;
        dma_free_coherent(gpu->wire, 0);
        return ret;
    }
    ret = virtgpu_init_display(gpu, scanouts);
    Assert(!ret, "display initialization failed (%d)\n", ret);
    gpu->chardev = CHARDEV_INIT(GPU_DEV, &virtgpu_fops, gpu);
    chardev_register(&gpu->chardev);
    pr_info("device: %ux%u, stride=%u, size=%llu, chunks=%u, dev=%u:%u\n",
            gpu->res.width, gpu->res.height, gpu->res.stride,
            (unsigned long long)gpu->res.size, gpu->res.nr_chunks,
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
