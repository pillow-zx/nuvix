#define pr_fmt(fmt) "virtio-gpu: " fmt

#include <nuvix/chardev.h>
#include <nuvix/dma.h>
#include <nuvix/errno.h>
#include <nuvix/fs.h>
#include <nuvix/math.h>
#include <nuvix/mm.h>
#include <nuvix/module.h>
#include <nuvix/page.h>
#include <nuvix/pgtable.h>
#include <nuvix/printk.h>
#include <nuvix/slab.h>
#include <nuvix/tick.h>
#include <nuvix/uaccess.h>
#include <nuvix/virtio.h>
#include <nuvix/virtqueue.h>

#define GPU_DEV             MKDEV(241, 0)
#define GPU_FORMAT_BGRX8888 1
#define GPU_GET_INFO        0x80184700U
#define GPU_FLUSH           0x00004701U

struct gpu_info {
    unsigned int width;
    unsigned int height;
    unsigned int stride;
    unsigned int format;
    unsigned long long size;
};

#define VIRTIO_GPU_QUEUE_SIZE                   16
#define VIRTIO_GPU_TIMEOUT_NS                   (30ULL * NSEC_PER_SEC)
#define VIRTIO_GPU_MAX_SCANOUTS                 16
#define VIRTIO_GPU_RESOURCE_ID                  1
#define VIRTIO_GPU_PIXEL_BYTES                  4
#define VIRTIO_GPU_FORMAT_BGRX8888              2
#define VIRTIO_GPU_CONFIG_EVENTS_READ           0
#define VIRTIO_GPU_CONFIG_EVENTS_CLEAR          4
#define VIRTIO_GPU_CONFIG_SCANOUTS              8
#define VIRTIO_GPU_EVENT_DISPLAY                1
#define VIRTIO_GPU_FLAG_FENCE                   1

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO         0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D       0x0101
#define VIRTIO_GPU_CMD_SET_SCANOUT              0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH           0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D      0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING  0x0106
#define VIRTIO_GPU_RESP_OK_NODATA               0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO         0x1101
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY       0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID  0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER   0x1205

struct virtio_gpu_hdr {
    uint32_t type;
    uint32_t flags;
    /* One outstanding command; fence ID and context fields stay zero. */
    uint8_t reserved[16];
};

struct virtio_gpu_rect {
    uint8_t reserved[8];
    uint32_t width;
    uint32_t height;
};

struct virtio_gpu_create {
    struct virtio_gpu_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

struct virtio_gpu_attach {
    struct virtio_gpu_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    struct {
        uint64_t addr;
        uint32_t length;
        uint8_t padding[4];
    } entry;
};

struct virtio_gpu_scanout {
    struct virtio_gpu_hdr hdr;
    struct virtio_gpu_rect rect;
    uint32_t scanout_id;
    uint32_t resource_id;
};

struct virtio_gpu_transfer {
    struct virtio_gpu_hdr hdr;
    struct virtio_gpu_rect rect;
    uint8_t reserved[8];
    uint32_t resource_id;
    uint8_t padding[4];
};

struct virtio_gpu_flush {
    struct virtio_gpu_hdr hdr;
    struct virtio_gpu_rect rect;
    uint32_t resource_id;
    uint8_t padding[4];
};

struct virtio_gpu_display_info {
    struct virtio_gpu_hdr hdr;
    struct {
        struct virtio_gpu_rect rect;
        uint32_t enabled;
        uint8_t reserved[4];
    } modes[VIRTIO_GPU_MAX_SCANOUTS];
};

struct virtio_gpu_wire {
    union {
        struct virtio_gpu_hdr hdr;
        struct virtio_gpu_create create;
        struct virtio_gpu_attach attach;
        struct virtio_gpu_scanout scanout;
        struct virtio_gpu_transfer transfer;
        struct virtio_gpu_flush flush;
    } request;
    union {
        struct virtio_gpu_hdr hdr;
        struct virtio_gpu_display_info display;
    } response;
};

struct virtio_gpu {
    struct virtio_dev *dev;
    struct chardev chardev;
    struct mutex lock;
    struct gpu_info info;
    void *pixels;
    dma_t pixels_dma;
    struct virtio_gpu_wire *wire;
    dma_t wire_dma;
};

static int virtio_gpu_command(struct virtio_gpu *gpu, uint32_t command,
                              unsigned int request_size, uint32_t response_type)
{
    struct virtqueue *vq = gpu->dev->vqs[0];
    struct virtio_gpu_hdr *response = &gpu->wire->response.hdr;
    unsigned int response_size =
        response_type == VIRTIO_GPU_RESP_OK_DISPLAY_INFO
            ? sizeof(gpu->wire->response.display)
            : sizeof(*response);
    struct vq_sg sg[] = {
        {gpu->wire_dma + offsetof(struct virtio_gpu_wire, request),
         request_size},
        {gpu->wire_dma + offsetof(struct virtio_gpu_wire, response),
         response_size},
    };
    uint64_t deadline;
    uint32_t written = 0;
    int ret;

    ret = time_deadline_from_ns(VIRTIO_GPU_TIMEOUT_NS, &deadline);
    if (ret)
        return ret;
    gpu->wire->request.hdr = (struct virtio_gpu_hdr){
        .type = command,
        .flags = VIRTIO_GPU_FLAG_FENCE,
    };
    ret = vq_add(vq, sg, 1, 1, gpu);
    Assert(!ret, "GPU control request lacked queue capacity (%d)\n", ret);
    vq_kick(vq);
    while (!vq_get_buf(vq, &written))
        Assert(time_read() < deadline, "GPU control request timed out\n");

    if (written < sizeof(*response) ||
        !(response->flags & VIRTIO_GPU_FLAG_FENCE))
        return -EIO;
    if (response->type == response_type)
        return written == response_size ? 0 : -EIO;
    switch (response->type) {
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

static int virtio_gpu_flush(struct virtio_gpu *gpu)
{
    struct virtio_gpu_rect rect = {
        .width = gpu->info.width,
        .height = gpu->info.height,
    };
    int ret;

    gpu->wire->request.transfer = (struct virtio_gpu_transfer){
        .rect = rect,
        .resource_id = VIRTIO_GPU_RESOURCE_ID,
    };
    ret = virtio_gpu_command(gpu, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
                             sizeof(gpu->wire->request.transfer),
                             VIRTIO_GPU_RESP_OK_NODATA);
    if (ret)
        return ret;
    gpu->wire->request.flush = (struct virtio_gpu_flush){
        .rect = rect,
        .resource_id = VIRTIO_GPU_RESOURCE_ID,
    };
    return virtio_gpu_command(gpu, VIRTIO_GPU_CMD_RESOURCE_FLUSH,
                              sizeof(gpu->wire->request.flush),
                              VIRTIO_GPU_RESP_OK_NODATA);
}

static int virtio_gpu_init_display(struct virtio_gpu *gpu,
                                   unsigned int num_scanouts)
{
    struct virtio_gpu_display_info *display = &gpu->wire->response.display;
    unsigned int scanout;
    int order, ret;

    ret = virtio_gpu_command(gpu, VIRTIO_GPU_CMD_GET_DISPLAY_INFO,
                             sizeof(struct virtio_gpu_hdr),
                             VIRTIO_GPU_RESP_OK_DISPLAY_INFO);
    if (ret)
        return ret;
    for (scanout = 0; scanout < num_scanouts; ++scanout)
        if (display->modes[scanout].enabled)
            break;
    if (scanout == num_scanouts)
        return -ENODEV;
    gpu->info.width = display->modes[scanout].rect.width;
    gpu->info.height = display->modes[scanout].rect.height;
    gpu->info.format = GPU_FORMAT_BGRX8888;
    if (!gpu->info.width || !gpu->info.height ||
        check_mul_overflow(gpu->info.width, VIRTIO_GPU_PIXEL_BYTES,
                           &gpu->info.stride))
        return -EIO;
    gpu->info.size = (uint64_t)gpu->info.stride * gpu->info.height;
    if (gpu->info.size > UINT32_MAX)
        return -E2BIG;
    for (order = 0; order < CONFIG_MAX_ORDER; ++order)
        if (gpu->info.size <= ((size_t)PAGE_SIZE << order))
            break;
    if (order == CONFIG_MAX_ORDER)
        return -E2BIG;
    gpu->pixels = dma_alloc_coherent(order, &gpu->pixels_dma);
    if (!gpu->pixels)
        return -ENOMEM;

    gpu->wire->request.create = (struct virtio_gpu_create){
        .resource_id = VIRTIO_GPU_RESOURCE_ID,
        .format = VIRTIO_GPU_FORMAT_BGRX8888,
        .width = gpu->info.width,
        .height = gpu->info.height,
    };
    ret = virtio_gpu_command(gpu, VIRTIO_GPU_CMD_RESOURCE_CREATE_2D,
                             sizeof(gpu->wire->request.create),
                             VIRTIO_GPU_RESP_OK_NODATA);
    if (ret)
        return ret;
    gpu->wire->request.attach = (struct virtio_gpu_attach){
        .resource_id = VIRTIO_GPU_RESOURCE_ID,
        .nr_entries = 1,
        .entry =
            {
                .addr = gpu->pixels_dma,
                .length = (uint32_t)gpu->info.size,
            },
    };
    ret = virtio_gpu_command(gpu, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING,
                             sizeof(gpu->wire->request.attach),
                             VIRTIO_GPU_RESP_OK_NODATA);
    if (ret)
        return ret;
    gpu->wire->request.scanout = (struct virtio_gpu_scanout){
        .rect = {.width = gpu->info.width, .height = gpu->info.height},
        .scanout_id = scanout,
        .resource_id = VIRTIO_GPU_RESOURCE_ID,
    };
    ret = virtio_gpu_command(gpu, VIRTIO_GPU_CMD_SET_SCANOUT,
                             sizeof(gpu->wire->request.scanout),
                             VIRTIO_GPU_RESP_OK_NODATA);
    return ret ? ret : virtio_gpu_flush(gpu);
}

static int virtio_gpu_mmap(struct file *file, struct vm_struct *vma)
{
    struct virtio_gpu *gpu = file->f_private;
    size_t size = vma->vm_end - vma->vm_start;

    if (!(vma->vm_flags & VM_SHARED) || vma->vm_offset > gpu->info.size ||
        size > gpu->info.size - vma->vm_offset)
        return -EINVAL;
    return pgtable_map_io(vma->vm_mm, vma->vm_start,
                          arch_dma_to_phys(gpu->pixels_dma) + vma->vm_offset,
                          size, vma->vm_flags);
}

static long virtio_gpu_ioctl(struct file *file, unsigned int cmd,
                             unsigned long arg)
{
    struct virtio_gpu *gpu = file->f_private;
    int ret;

    switch (cmd) {
    case GPU_GET_INFO:
        return copy_to_user((void *)arg, &gpu->info, sizeof(gpu->info))
                   ? -EFAULT
                   : 0;
    case GPU_FLUSH:
        if (!FILE_WRITE(file))
            return -EBADF;
        ret = mutex_lock_interruptible(&gpu->lock);
        if (ret)
            return ret;
        ret = virtio_gpu_flush(gpu);
        mutex_unlock(&gpu->lock);
        return ret;
    default:
        return -ENOTTY;
    }
}

static const struct file_operations virtio_gpu_fops = {
    .ioctl = virtio_gpu_ioctl,
    .mmap = virtio_gpu_mmap,
};

static void virtio_gpu_config_changed(struct virtio_dev *dev)
{
    uint32_t events;
    int ret;

    ret = virtio_config_read32(dev, VIRTIO_GPU_CONFIG_EVENTS_READ, &events);
    Assert(!ret, "GPU event read failed (%d)\n", ret);
    if (events & VIRTIO_GPU_EVENT_DISPLAY) {
        ret = virtio_config_write32(dev, VIRTIO_GPU_CONFIG_EVENTS_CLEAR,
                                    VIRTIO_GPU_EVENT_DISPLAY);
        Assert(!ret, "GPU event clear failed (%d)\n", ret);
        pr_warn("display configuration changed; retaining boot mode\n");
    }
}

static int virtio_gpu_probe(struct device *device)
{
    struct virtio_dev *dev = to_virtio_device(device);
    struct virtio_gpu *gpu __free(kfree) = NULL;
    uint32_t num_scanouts;
    int ret;

    gpu = kzalloc(sizeof(*gpu));
    if (!gpu)
        return -ENOMEM;
    ret = virtio_config_read32(dev, VIRTIO_GPU_CONFIG_SCANOUTS, &num_scanouts);
    if (ret)
        return ret;
    if (!num_scanouts || num_scanouts > VIRTIO_GPU_MAX_SCANOUTS)
        return -EIO;
    ret = vq_create(dev, VIRTIO_GPU_QUEUE_SIZE, NULL);
    if (ret)
        return ret;
    ret = vq_create(dev, VIRTIO_GPU_QUEUE_SIZE, NULL);
    if (ret)
        return ret;
    gpu->wire = dma_alloc_coherent(0, &gpu->wire_dma);
    if (!gpu->wire)
        return -ENOMEM;
    gpu->dev = dev;
    mutex_init(&gpu->lock);
    device->drvdata = gpu;
    ret = virtio_dev_ready(dev);
    if (ret) {
        device->drvdata = NULL;
        dma_free_coherent(gpu->wire, 0);
        return ret;
    }
    ret = virtio_gpu_init_display(gpu, num_scanouts);
    Assert(!ret, "GPU display initialization failed (%d)\n", ret);
    gpu->chardev = (struct chardev){
        .dev = GPU_DEV,
        .fops = &virtio_gpu_fops,
        .private = gpu,
    };
    chardev_register(&gpu->chardev);
    pr_info("device: %ux%u, stride=%u, size=%llu, dev=%u:%u\n", gpu->info.width,
            gpu->info.height, gpu->info.stride, gpu->info.size, MAJOR(GPU_DEV),
            MINOR(GPU_DEV));
    keep(gpu);
    return 0;
}

static struct virtio_driver virtio_gpu_driver = {
    .drv =
        {
            .name = "virtio-gpu",
            .bus = &virtio_bus,
            .probe = virtio_gpu_probe,
        },
    .device_id = VIRTIO_ID_GPU,
    .config_changed = virtio_gpu_config_changed,
};

MODULE_DRIVER_INIT(virtio_gpu, 84);
