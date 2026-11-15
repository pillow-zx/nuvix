#ifndef _NUVIX_VIRTSND_H
#define _NUVIX_VIRTSND_H

#include <nuvix/ctypes.h>

#define VIRTIO_SND_CONFIG_STREAMS 4
#define VIRTIO_SND_VQ_CONTROL     0
#define VIRTIO_SND_VQ_TX          2
#define VIRTIO_SND_VQ_COUNT       4

#define VIRTIO_SND_R_PCM_INFO     0x0100
#define VIRTIO_SND_R_SET_PARAMS   0x0101
#define VIRTIO_SND_R_PREPARE      0x0102
#define VIRTIO_SND_R_RELEASE      0x0103
#define VIRTIO_SND_R_START        0x0104
#define VIRTIO_SND_R_STOP         0x0105
#define VIRTIO_SND_FMT_S16        5
#define VIRTIO_SND_RATE_44100     6
#define VIRTIO_SND_S_OK           0x8000
#define VIRTIO_SND_S_BAD_MSG      0x8001
#define VIRTIO_SND_S_NOT_SUPP     0x8002
#define VIRTIO_SND_D_OUTPUT       0
#define VIRTIO_SND_D_INPUT        1

struct virtsnd_query {
    uint32_t code;
    uint32_t start_id;
    uint32_t count;
    uint32_t size;
};

struct virtsnd_pcm_info {
    uint8_t reserved[8];
    uint64_t formats;
    uint64_t rates;
    uint8_t direction;
    uint8_t channels_min;
    uint8_t channels_max;
    uint8_t padding[5];
};

struct virtsnd_cmd {
    uint32_t code;
    uint32_t stream_id;
};

struct virtsnd_params {
    struct virtsnd_cmd hdr;
    uint32_t buffer_bytes;
    uint32_t period_bytes;
    uint8_t reserved[4];
    uint8_t channels;
    uint8_t format;
    uint8_t rate;
    uint8_t padding[1];
};

struct virtsnd_pcm_status {
    uint32_t code;
    uint32_t latency_bytes;
};

#endif
