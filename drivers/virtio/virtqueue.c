#include <nuvix/virtqueue.h>
#include <nuvix/assert.h>
#include <nuvix/errno.h>
#include <nuvix/macro.h>
#include <nuvix/slab.h>

void vq_destroy(struct virtqueue *vq)
{
    dma_free_coherent(vq->desc, 0);
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
