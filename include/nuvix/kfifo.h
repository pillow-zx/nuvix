#ifndef _NUVIX_KFIFO_H
#define _NUVIX_KFIFO_H

#include <nuvix/ctypes.h>
#include <nuvix/errno.h>
#include <nuvix/math.h>
#include <nuvix/string.h>

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
    return fifo->in - fifo->out;
}

static inline unsigned int kfifo_avail(const struct kfifo *fifo)
{
    return kfifo_size(fifo) - kfifo_len(fifo);
}

static inline bool kfifo_is_empty(const struct kfifo *fifo)
{
    return fifo->in == fifo->out;
}

static inline bool kfifo_is_full(const struct kfifo *fifo)
{
    return kfifo_len(fifo) == kfifo_size(fifo);
}

static inline bool kfifo_put(struct kfifo *fifo, uint8_t value)
{
    if (kfifo_is_full(fifo))
        return false;
    fifo->data[fifo->in & fifo->mask] = value;
    ++fifo->in;
    return true;
}

static inline bool kfifo_get(struct kfifo *fifo, uint8_t *value)
{
    if (kfifo_is_empty(fifo))
        return false;
    *value = fifo->data[fifo->out & fifo->mask];
    ++fifo->out;
    return true;
}

/* Kernel buffers must not overlap FIFO storage; returns bytes transferred. */
static inline unsigned int kfifo_in(struct kfifo *fifo, const void *buffer,
                                    unsigned int len)
{
    unsigned int offset = fifo->in & fifo->mask;
    unsigned int first;
    const uint8_t *src = buffer;

    len = MIN(len, kfifo_avail(fifo));
    if (!len)
        return 0;
    first = MIN(len, kfifo_size(fifo) - offset);
    memcpy(fifo->data + offset, src, first);
    memcpy(fifo->data, src + first, len - first);
    fifo->in += len;
    return len;
}

static inline unsigned int kfifo_out(struct kfifo *fifo, void *buffer,
                                     unsigned int len)
{
    unsigned int offset = fifo->out & fifo->mask;
    unsigned int first;
    uint8_t *dst = buffer;

    len = MIN(len, kfifo_len(fifo));
    if (!len)
        return 0;
    first = MIN(len, kfifo_size(fifo) - offset);
    memcpy(dst, fifo->data + offset, first);
    memcpy(dst + first, fifo->data, len - first);
    fifo->out += len;
    return len;
}

#endif /* ifndef _NUVIX_KFIFO_H */
