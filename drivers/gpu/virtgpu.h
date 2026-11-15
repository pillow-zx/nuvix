#ifndef _NUVIX_VIRTGPU_H
#define _NUVIX_VIRTGPU_H

#include <nuvix/ctypes.h>

#define VIRTIO_GPU_MAX_SCANOUTS                 16
#define VIRTIO_GPU_FORMAT_BGRX8888              2
#define VIRTIO_GPU_CONFIG_SCANOUTS              8
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

struct virtgpu_hdr {
    uint32_t type;
    uint32_t flags;
    uint8_t reserved[16];
};

struct virtgpu_rect {
    uint8_t reserved[8];
    uint32_t width;
    uint32_t height;
};

struct virtgpu_create {
    struct virtgpu_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

struct virtgpu_entry {
    uint64_t addr;
    uint32_t length;
    uint8_t padding[4];
};

struct virtgpu_attach {
    struct virtgpu_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    struct virtgpu_entry entries[];
};

struct virtgpu_scanout {
    struct virtgpu_hdr hdr;
    struct virtgpu_rect rect;
    uint32_t scanout_id;
    uint32_t resource_id;
};

struct virtgpu_transfer {
    struct virtgpu_hdr hdr;
    struct virtgpu_rect rect;
    uint8_t reserved[8];
    uint32_t resource_id;
    uint8_t padding[4];
};

struct virtgpu_flush {
    struct virtgpu_hdr hdr;
    struct virtgpu_rect rect;
    uint32_t resource_id;
    uint8_t padding[4];
};

struct virtgpu_display_info {
    struct virtgpu_hdr hdr;
    struct {
        struct virtgpu_rect rect;
        uint32_t enabled;
        uint8_t reserved[4];
    } modes[VIRTIO_GPU_MAX_SCANOUTS];
};

#endif
