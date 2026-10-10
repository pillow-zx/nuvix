#ifndef _NUVIX_VIRTQUEUE_H
#define _NUVIX_VIRTQUEUE_H

#include <nuvix/dma.h>
#include <nuvix/spinlock.h>

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2
#define VIRTQUEUE_MAX_SIZE 32768

struct virtio_dev;

struct vq_sg {
    dma_addr_t addr;
    uint32_t len;
};

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
};

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
};

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[];
};

struct virtqueue {
    struct virtio_dev *dev;
    uint16_t index;
    uint16_t num;
    void (*callback)(struct virtqueue *vq);
    spinlock_t lock;
    struct vring_desc *desc;
    struct vring_avail *avail;
    struct vring_used *used;
    dma_addr_t desc_dma;
    dma_addr_t avail_dma;
    dma_addr_t used_dma;
    void *ring_mem;
    int ring_order;
    uint16_t free_head;
    uint16_t num_free;
    uint16_t avail_idx;
    uint16_t last_used_idx;
    void *cookies[];
};

struct virtqueue *virtqueue_create(struct virtio_dev *dev, unsigned int index,
                                   unsigned int num,
                                   void (*callback)(struct virtqueue *));
void virtqueue_destroy(struct virtqueue *vq);

int vq_add(struct virtqueue *vq, const struct vq_sg *sg, unsigned int n_out,
           unsigned int n_in, void *cookie);
void vq_kick(struct virtqueue *vq);
void *vq_get_buf(struct virtqueue *vq, uint32_t *len);

#endif /* _NUVIX_VIRTQUEUE_H */
