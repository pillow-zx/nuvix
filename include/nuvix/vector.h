#ifndef _NUVIX_VECTOR_H
#define _NUVIX_VECTOR_H

#include <nuvix/ctypes.h>
#include <nuvix/errno.h>
#include <nuvix/slab.h>
#include <nuvix/string.h>
#include <nuvix/macro.h>

/*
 * Caller-serialized, owning array of byte-copyable elements, following kvec's
 * length/capacity/storage layout. Growth invalidates pointers into storage.
 * Resize zeroes new elements; shrinking/destroying never frees their pointees.
 * Allocation errors leave the vector unchanged. Do not shallow-copy ownership.
 */
#define vector(type)                                                           \
    struct {                                                                   \
        size_t size, capacity;                                                 \
        type *data;                                                            \
    }

#define vector_init(v)                                                         \
    do {                                                                       \
        auto __vi = &(v);                                                      \
        __vi->size = __vi->capacity = 0;                                       \
        __vi->data = NULL;                                                     \
    } while (0)

#define vector_destroy(v)                                                      \
    do {                                                                       \
        auto __vd = &(v);                                                      \
        kfree(__vd->data);                                                     \
        vector_init(*__vd);                                                    \
    } while (0)

#define vector_size(v)     ((v).size)
#define vector_capacity(v) ((v).capacity)
/* Requires i < vector_size(v). */
#define vector_at(v, i)    ((v).data[(i)])

/* Reserve/resize/push return 0 or a negative errno. */
#define vector_reserve(v, count)                                               \
    ({                                                                         \
        auto __vr = &(v);                                                      \
        size_t __vr_count = (count);                                           \
        size_t __vr_max = SIZE_MAX / sizeof(*__vr->data);                      \
        int __vr_ret = 0;                                                      \
                                                                               \
        if (__vr_count > __vr_max) {                                           \
            __vr_ret = -EOVERFLOW;                                             \
        } else if (__vr_count > __vr->capacity) {                              \
            size_t __vr_capacity = __vr->capacity;                             \
                                                                               \
            if (!__vr_capacity)                                                \
                __vr_capacity = __vr_max < 2 ? __vr_max : 2;                   \
            else                                                               \
                __vr_capacity = __vr_capacity > __vr_max / 2                   \
                                    ? __vr_max                                 \
                                    : __vr_capacity * 2;                       \
            if (__vr_capacity < __vr_count)                                    \
                __vr_capacity = __vr_count;                                    \
            void *__vr_data = kmalloc(__vr_capacity * sizeof(*__vr->data));    \
                                                                               \
            if (!__vr_data) {                                                  \
                __vr_ret = -ENOMEM;                                            \
            } else {                                                           \
                if (__vr->size)                                                \
                    memcpy(__vr_data, __vr->data,                              \
                           __vr->size * sizeof(*__vr->data));                  \
                kfree(__vr->data);                                             \
                __vr->data = __vr_data;                                        \
                __vr->capacity = __vr_capacity;                                \
            }                                                                  \
        }                                                                      \
        __vr_ret;                                                              \
    })

#define vector_resize(v, count)                                                \
    ({                                                                         \
        auto __vz = &(v);                                                      \
        size_t __vz_count = (count);                                           \
        int __vz_ret = vector_reserve(*__vz, __vz_count);                      \
                                                                               \
        if (!__vz_ret) {                                                       \
            if (__vz_count > __vz->size)                                       \
                memset(__vz->data + __vz->size, 0,                             \
                       (__vz_count - __vz->size) * sizeof(*__vz->data));       \
            __vz->size = __vz_count;                                           \
        }                                                                      \
        __vz_ret;                                                              \
    })

#define vector_push(v, value)                                                  \
    ({                                                                         \
        auto __vp = &(v);                                                      \
        TYPEOF(*__vp->data) __vp_value = (value);                              \
        int __vp_ret = __vp->size == SIZE_MAX                                  \
                           ? -EOVERFLOW                                        \
                           : vector_reserve(*__vp, __vp->size + 1);            \
                                                                               \
        if (!__vp_ret)                                                         \
            __vp->data[__vp->size++] = __vp_value;                             \
        __vp_ret;                                                              \
    })

#endif /* _NUVIX_VECTOR_H */
