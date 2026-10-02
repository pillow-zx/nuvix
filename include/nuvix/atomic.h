#ifndef _NUVIX_ATOMIC_H
#define _NUVIX_ATOMIC_H

#include <nuvix/ctypes.h>
#include <nuvix/assert.h>

typedef struct {
    int32_t counter __attribute__((aligned(4)));
} atomic_t;

typedef struct {
    int64_t counter __attribute__((aligned(8)));
} atomic64_t;

#define ATOMIC_INIT(value)   {(value)}
#define ATOMIC64_INIT(value) {(value)}

static_assert(__atomic_always_lock_free(4, 0),
              "32-bit atomics must be lock-free");
static_assert(__atomic_always_lock_free(8, 0),
              "64-bit atomics must be lock-free");
static_assert(__atomic_always_lock_free(sizeof(void *), 0),
              "pointer atomics must be lock-free");

#define __NUVIX_ATOMIC_BASE(prefix, type, value_type)                          \
    static inline value_type prefix##_read(const type *v)                      \
    {                                                                          \
        return __atomic_load_n(&v->counter, __ATOMIC_RELAXED);                 \
    }                                                                          \
    static inline value_type prefix##_read_acquire(const type *v)              \
    {                                                                          \
        return __atomic_load_n(&v->counter, __ATOMIC_ACQUIRE);                 \
    }                                                                          \
    static inline void prefix##_set(type *v, value_type value)                 \
    {                                                                          \
        __atomic_store_n(&v->counter, value, __ATOMIC_RELAXED);                \
    }                                                                          \
    static inline void prefix##_set_release(type *v, value_type value)         \
    {                                                                          \
        __atomic_store_n(&v->counter, value, __ATOMIC_RELEASE);                \
    }                                                                          \
    static inline void prefix##_inc(type *v)                                   \
    {                                                                          \
        prefix##_add(1, v);                                                    \
    }                                                                          \
    static inline void prefix##_dec(type *v)                                   \
    {                                                                          \
        prefix##_sub(1, v);                                                    \
    }

#define __NUVIX_ATOMIC_VOID(prefix, type, value_type, op)                      \
    static inline void prefix##_##op(value_type value, type *v)                \
    {                                                                          \
        (void)__atomic_fetch_##op(&v->counter, value, __ATOMIC_RELAXED);       \
    }

#define __NUVIX_ATOMIC_FETCH(prefix, type, value_type, op, suffix, order)      \
    static inline value_type prefix##_fetch_##op##suffix(value_type value,     \
                                                         type *v)              \
    {                                                                          \
        return __atomic_fetch_##op(&v->counter, value, order);                 \
    }

#define __NUVIX_ATOMIC_RMW(prefix, type, value_type, suffix, order)            \
    __NUVIX_ATOMIC_FETCH(prefix, type, value_type, add, suffix, order)         \
    __NUVIX_ATOMIC_FETCH(prefix, type, value_type, sub, suffix, order)         \
    __NUVIX_ATOMIC_FETCH(prefix, type, value_type, and, suffix, order)         \
    __NUVIX_ATOMIC_FETCH(prefix, type, value_type, or, suffix, order)          \
    __NUVIX_ATOMIC_FETCH(prefix, type, value_type, xor, suffix, order)         \
    static inline value_type prefix##_add_return##suffix(value_type value,     \
                                                         type *v)              \
    {                                                                          \
        return __atomic_add_fetch(&v->counter, value, order);                  \
    }                                                                          \
    static inline value_type prefix##_sub_return##suffix(value_type value,     \
                                                         type *v)              \
    {                                                                          \
        return __atomic_sub_fetch(&v->counter, value, order);                  \
    }                                                                          \
    static inline value_type prefix##_inc_return##suffix(type *v)              \
    {                                                                          \
        return prefix##_add_return##suffix(1, v);                              \
    }                                                                          \
    static inline value_type prefix##_dec_return##suffix(type *v)              \
    {                                                                          \
        return prefix##_sub_return##suffix(1, v);                              \
    }                                                                          \
    static inline value_type prefix##_xchg##suffix(type *v, value_type value)  \
    {                                                                          \
        return __atomic_exchange_n(&v->counter, value, order);                 \
    }                                                                          \
    static inline bool prefix##_try_cmpxchg##suffix(type *v, value_type *old,  \
                                                    value_type value)          \
    {                                                                          \
        return __atomic_compare_exchange_n(&v->counter, old, value, false,     \
                                           order, __ATOMIC_RELAXED);           \
    }                                                                          \
    static inline value_type prefix##_cmpxchg##suffix(type *v, value_type old, \
                                                      value_type value)        \
    {                                                                          \
        (void)prefix##_try_cmpxchg##suffix(v, &old, value);                    \
        return old;                                                            \
    }

#define __NUVIX_ATOMIC_DEFINE(prefix, type, value_type)                        \
    __NUVIX_ATOMIC_VOID(prefix, type, value_type, add)                         \
    __NUVIX_ATOMIC_VOID(prefix, type, value_type, sub)                         \
    __NUVIX_ATOMIC_VOID(prefix, type, value_type, and)                         \
    __NUVIX_ATOMIC_VOID(prefix, type, value_type, or)                          \
    __NUVIX_ATOMIC_VOID(prefix, type, value_type, xor)                         \
    __NUVIX_ATOMIC_BASE(prefix, type, value_type)                              \
    __NUVIX_ATOMIC_RMW(prefix, type, value_type, , __ATOMIC_SEQ_CST)           \
    __NUVIX_ATOMIC_RMW(prefix, type, value_type, _relaxed, __ATOMIC_RELAXED)   \
    __NUVIX_ATOMIC_RMW(prefix, type, value_type, _acquire, __ATOMIC_ACQUIRE)   \
    __NUVIX_ATOMIC_RMW(prefix, type, value_type, _release, __ATOMIC_RELEASE)

__NUVIX_ATOMIC_DEFINE(atomic, atomic_t, int32_t)
__NUVIX_ATOMIC_DEFINE(atomic64, atomic64_t, int64_t)

#undef __NUVIX_ATOMIC_DEFINE
#undef __NUVIX_ATOMIC_RMW
#undef __NUVIX_ATOMIC_FETCH
#undef __NUVIX_ATOMIC_BASE
#undef __NUVIX_ATOMIC_VOID

#define __nuvix_atomic_check(ptr)                                              \
    static_assert(                                                             \
        (sizeof(*(ptr)) == 4 || sizeof(*(ptr)) == 8) &&                        \
            __atomic_always_lock_free(sizeof(*(ptr)), 0),                      \
        "atomic operand must be a lock-free 32/64-bit scalar or pointer")

#define __nuvix_xchg(ptr, value, order)                                        \
    ({                                                                         \
        __auto_type __atomic_ptr = (ptr);                                      \
        __nuvix_atomic_check(__atomic_ptr);                                    \
        __atomic_exchange_n(__atomic_ptr, (value), order);                     \
    })

#define __nuvix_cmpxchg(ptr, old, value, order)                                \
    ({                                                                         \
        __auto_type __atomic_ptr = (ptr);                                      \
        __nuvix_atomic_check(__atomic_ptr);                                    \
        __auto_type __atomic_old = (__typeof__(*__atomic_ptr))(old);           \
        (void)__atomic_compare_exchange_n(__atomic_ptr, &__atomic_old,         \
                                          (value), false, order,               \
                                          __ATOMIC_RELAXED);                   \
        __atomic_old;                                                          \
    })

#define __nuvix_try_cmpxchg(ptr, old, value, order)                            \
    ({                                                                         \
        __auto_type __atomic_ptr = (ptr);                                      \
        __nuvix_atomic_check(__atomic_ptr);                                    \
        __atomic_compare_exchange_n(__atomic_ptr, (old), (value), false,       \
                                    order, __ATOMIC_RELAXED);                  \
    })

#define xchg(ptr, value)         __nuvix_xchg(ptr, value, __ATOMIC_SEQ_CST)
#define xchg_relaxed(ptr, value) __nuvix_xchg(ptr, value, __ATOMIC_RELAXED)
#define xchg_acquire(ptr, value) __nuvix_xchg(ptr, value, __ATOMIC_ACQUIRE)
#define xchg_release(ptr, value) __nuvix_xchg(ptr, value, __ATOMIC_RELEASE)
#define cmpxchg(ptr, old, value)                                               \
    __nuvix_cmpxchg(ptr, old, value, __ATOMIC_SEQ_CST)
#define cmpxchg_relaxed(ptr, old, value)                                       \
    __nuvix_cmpxchg(ptr, old, value, __ATOMIC_RELAXED)
#define cmpxchg_acquire(ptr, old, value)                                       \
    __nuvix_cmpxchg(ptr, old, value, __ATOMIC_ACQUIRE)
#define cmpxchg_release(ptr, old, value)                                       \
    __nuvix_cmpxchg(ptr, old, value, __ATOMIC_RELEASE)
#define try_cmpxchg(ptr, old, value)                                           \
    __nuvix_try_cmpxchg(ptr, old, value, __ATOMIC_SEQ_CST)
#define try_cmpxchg_relaxed(ptr, old, value)                                   \
    __nuvix_try_cmpxchg(ptr, old, value, __ATOMIC_RELAXED)
#define try_cmpxchg_acquire(ptr, old, value)                                   \
    __nuvix_try_cmpxchg(ptr, old, value, __ATOMIC_ACQUIRE)
#define try_cmpxchg_release(ptr, old, value)                                   \
    __nuvix_try_cmpxchg(ptr, old, value, __ATOMIC_RELEASE)

#define __nuvix_atomic_ptr_check(ptr)                                          \
    static_assert(__builtin_classify_type(*(ptr)) == 5,                        \
                  "pointer slot required");                                    \
    __nuvix_atomic_check(ptr)

#define __nuvix_atomic_ptr_read(ptr, order)                                    \
    ({                                                                         \
        __auto_type __atomic_ptr = (ptr);                                      \
        __nuvix_atomic_ptr_check(__atomic_ptr);                                \
        __atomic_load_n(__atomic_ptr, order);                                  \
    })

#define __nuvix_atomic_ptr_set(ptr, value, order)                              \
    do {                                                                       \
        __auto_type __atomic_ptr = (ptr);                                      \
        __nuvix_atomic_ptr_check(__atomic_ptr);                                \
        __atomic_store_n(__atomic_ptr, (value), order);                        \
    } while (0)

#define atomic_ptr_read(ptr) __nuvix_atomic_ptr_read(ptr, __ATOMIC_RELAXED)
#define atomic_ptr_read_acquire(ptr)                                           \
    __nuvix_atomic_ptr_read(ptr, __ATOMIC_ACQUIRE)
#define atomic_ptr_set(ptr, value)                                             \
    __nuvix_atomic_ptr_set(ptr, value, __ATOMIC_RELAXED)
#define atomic_ptr_set_release(ptr, value)                                     \
    __nuvix_atomic_ptr_set(ptr, value, __ATOMIC_RELEASE)

#endif /* _NUVIX_ATOMIC_H */
