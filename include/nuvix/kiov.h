#ifndef _NUVIX_KIOV_H
#define _NUVIX_KIOV_H

#include <nuvix/ctypes.h>
#include <nuvix/compiler.h>

struct iovec {
    void *iov_base;
    size_t iov_len;
};

struct kiov {
    const struct iovec *vec;
    size_t segs;
    size_t offset;
    size_t count;
    bool user;
};

static __always_inline void kiov_init(struct kiov *iov, const struct iovec *vec,
                                      size_t nr, size_t count, bool user)
{
    *iov = (struct kiov){
        .vec = vec,
        .segs = nr,
        .count = count,
        .user = user,
    };
}

void kiov_advance(struct kiov *iov, size_t count);

size_t copy_to_kiov(const void *buffer, size_t count, struct kiov *iov);
size_t copy_from_kiov(void *buffer, size_t count, struct kiov *iov);

#endif
