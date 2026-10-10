#include <nuvix/virtqueue.h>
#include <nuvix/virtio.h>
#include <nuvix/assert.h>
#include <nuvix/errno.h>
#include <nuvix/macro.h>
#include <nuvix/math.h>
#include <nuvix/page.h>
#include <nuvix/slab.h>

#define VRING_USED_ALIGN 4

struct virtqueue *virtqueue_create(struct virtio_dev *dev, unsigned int index,
                                   unsigned int num,
                                   void (*callback)(struct virtqueue *))
{
    struct virtqueue *vq __free(kfree) = NULL;
    size_t avail_offset, used_offset, bytes;
    dma_addr_t dma_addr;
    int order = 0;

    vq = kzalloc(sizeof(*vq) + num * sizeof(vq->cookies[0]));
    if (!vq)
        return ERR_PTR(-ENOMEM);
    avail_offset = num * sizeof(struct vring_desc);
    used_offset = ALIGN_UP(avail_offset + sizeof(struct vring_avail) +
                               (num + 1) * sizeof(uint16_t),
                           (size_t)VRING_USED_ALIGN);
    bytes = used_offset + sizeof(struct vring_used) +
            num * sizeof(struct vring_used_elem) + sizeof(uint16_t);
    while (((size_t)PAGE_SIZE << order) < bytes)
        ++order;
    vq->ring_mem = dma_alloc_coherent(order, &dma_addr);
    if (!vq->ring_mem)
        return ERR_PTR(-ENOMEM);
    vq->dev = dev;
    vq->index = index;
    vq->num = num;
    vq->callback = callback;
    spin_lock_init(&vq->lock);
    vq->desc_dma = dma_addr;
    vq->avail_dma = dma_addr + avail_offset;
    vq->used_dma = dma_addr + used_offset;
    vq->ring_order = order;
    vq->desc = vq->ring_mem;
    vq->avail = (void *)((char *)vq->ring_mem + avail_offset);
    vq->used = (void *)((char *)vq->ring_mem + used_offset);
    vq->num_free = num;
    for (unsigned int i = 0; i < num; ++i)
        vq->desc[i].next = i + 1;
    return move(vq);
}

void virtqueue_destroy(struct virtqueue *vq)
{
    if (!vq)
        return;
    Assert(vq->num_free == vq->num,
           "destroying virtqueue with outstanding buffers\n");
    dma_free_coherent(vq->ring_mem, vq->ring_order);
    kfree(vq);
}

int vq_add(struct virtqueue *vq, const struct vq_sg *sg, unsigned int n_out,
           unsigned int n_in, void *cookie)
{
    unsigned int count = n_out + n_in;
    unsigned int head, next;
    unsigned long flags;

    spin_lock_irqsave(&vq->lock, flags);
    if (count > vq->num_free) {
        spin_unlock_irqrestore(&vq->lock, flags);
        return -ENOSPC;
    }
    head = next = vq->free_head;
    for (unsigned int i = 0; i < count; ++i) {
        struct vring_desc *desc = &vq->desc[next];

        next = desc->next;
        desc->addr = sg[i].addr;
        desc->len = sg[i].len;
        desc->flags = i < n_out ? 0 : VRING_DESC_F_WRITE;
        if (i + 1 < count)
            desc->flags |= VRING_DESC_F_NEXT;
    }
    vq->free_head = next;
    vq->num_free -= count;
    vq->cookies[head] = cookie;
    vq->avail->ring[vq->avail_idx & (vq->num - 1)] = head;
    dma_wmb();
    WRITE_ONCE(vq->avail->idx, ++vq->avail_idx);
    spin_unlock_irqrestore(&vq->lock, flags);
    return 0;
}

void vq_kick(struct virtqueue *vq)
{
    unsigned long flags;

    spin_lock_irqsave(&vq->lock, flags);
    dma_wmb();
    vq->dev->ops->notify(vq->dev, vq);
    spin_unlock_irqrestore(&vq->lock, flags);
}

void *vq_get_buf(struct virtqueue *vq, uint32_t *len)
{
    unsigned long flags;
    struct vring_used_elem *elem;
    unsigned int head, tail;
    uint16_t pending;
    void *cookie = NULL;

    spin_lock_irqsave(&vq->lock, flags);
    pending = (uint16_t)(READ_ONCE(vq->used->idx) - vq->last_used_idx);
    if (!pending)
        goto out;
    dma_rmb();
    Assert(pending <= (uint16_t)(vq->avail_idx - vq->last_used_idx),
           "virtqueue used index overrun\n");
    elem = &vq->used->ring[vq->last_used_idx & (vq->num - 1)];
    head = READ_ONCE(elem->id);
    Assert(head < vq->num && vq->cookies[head],
           "invalid virtqueue used head %u\n", head);
    cookie = vq->cookies[head];
    vq->cookies[head] = NULL;
    *len = READ_ONCE(elem->len);
    tail = head;
    while (vq->desc[tail].flags & VRING_DESC_F_NEXT) {
        ++vq->num_free;
        tail = vq->desc[tail].next;
    }
    ++vq->num_free;
    vq->desc[tail].next = vq->free_head;
    vq->free_head = head;
    ++vq->last_used_idx;
out:
    spin_unlock_irqrestore(&vq->lock, flags);
    return cookie;
}
