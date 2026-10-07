#ifndef _NUVIX_KFIFO_H
#define _NUVIX_KFIFO_H

#include <nuvix/atomic.h>
#include <nuvix/ctypes.h>
#include <nuvix/errno.h>
#include <nuvix/math.h>
#include <nuvix/string.h>

/*
 * One concurrent producer and one concurrent consumer need no external lock.
 * Serialize additional producers with each other, and consumers with each other.
 * Initialize before publication; init/reset require exclusive access.
 * Queries are transient snapshots; other observers need synchronization for a
 * consistent length. Do not access in/out directly during concurrent use.
 */
struct kfifo {
    unsigned int in;
    unsigned int out;
    unsigned int mask;
    uint8_t *data;
};

static inline int kfifo_init(struct kfifo *fifo, void *buffer,
                             unsigned int size)
{
    if (size < 2 || !IS_POWER_OF_2(size))
        return -EINVAL;
    fifo->in = 0;
    fifo->out = 0;
    fifo->mask = size - 1;
    fifo->data = buffer;
    return 0;
}

static inline void kfifo_reset(struct kfifo *fifo)
{
    fifo->in = fifo->out = 0;
}

static inline unsigned int kfifo_size(const struct kfifo *fifo)
{
    return fifo->mask + 1;
}

static inline unsigned int kfifo_len(const struct kfifo *fifo)
{
    unsigned int out = load_acquire(&fifo->out);
    unsigned int in = load_acquire(&fifo->in);

    return in - out;
}

static inline unsigned int kfifo_avail(const struct kfifo *fifo)
{
    return kfifo_size(fifo) - kfifo_len(fifo);
}

static inline bool kfifo_is_empty(const struct kfifo *fifo)
{
    return kfifo_len(fifo) == 0;
}

static inline bool kfifo_is_full(const struct kfifo *fifo)
{
    return kfifo_len(fifo) == kfifo_size(fifo);
}

static inline bool kfifo_put(struct kfifo *fifo, uint8_t value)
{
    unsigned int in = load_relaxed(&fifo->in);
    unsigned int out = load_acquire(&fifo->out);

    if (in - out == kfifo_size(fifo))
        return false;
    fifo->data[in & fifo->mask] = value;
    store_release(&fifo->in, in + 1);
    return true;
}

static inline bool kfifo_get(struct kfifo *fifo, uint8_t *value)
{
    unsigned int out = load_relaxed(&fifo->out);
    unsigned int in = load_acquire(&fifo->in);

    if (in == out)
        return false;
    *value = fifo->data[out & fifo->mask];
    store_release(&fifo->out, out + 1);
    return true;
}

/* Kernel buffers must not overlap FIFO storage; returns bytes transferred. */
static inline unsigned int kfifo_in(struct kfifo *fifo, const void *buffer,
                                    unsigned int len)
{
    unsigned int in = load_relaxed(&fifo->in);
    unsigned int out = load_acquire(&fifo->out);
    unsigned int offset = in & fifo->mask;
    unsigned int first;
    const uint8_t *src = buffer;

    len = MIN(len, kfifo_size(fifo) - (in - out));
    if (!len)
        return 0;
    first = MIN(len, kfifo_size(fifo) - offset);
    memcpy(fifo->data + offset, src, first);
    memcpy(fifo->data, src + first, len - first);
    store_release(&fifo->in, in + len);
    return len;
}

static inline unsigned int kfifo_out(struct kfifo *fifo, void *buffer,
                                     unsigned int len)
{
    unsigned int out = load_relaxed(&fifo->out);
    unsigned int in = load_acquire(&fifo->in);
    unsigned int offset = out & fifo->mask;
    unsigned int first;
    uint8_t *dst = buffer;

    len = MIN(len, in - out);
    if (!len)
        return 0;
    first = MIN(len, kfifo_size(fifo) - offset);
    memcpy(dst, fifo->data + offset, first);
    memcpy(dst + first, fifo->data, len - first);
    store_release(&fifo->out, out + len);
    return len;
}

#endif /* ifndef _NUVIX_KFIFO_H */
