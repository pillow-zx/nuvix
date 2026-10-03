#ifndef _NUVIX_REFCOUNT_H
#define _NUVIX_REFCOUNT_H

#include <nuvix/atomic.h>
#include <nuvix/macro.h>

typedef struct {
    atomic_t refs;
} refcount_t;

#define REFCOUNT_MAX         INT32_MAX
#define REFCOUNT_INIT(value) {.refs = ATOMIC_INIT(value)}

/*
 * Counts must be in [0, REFCOUNT_MAX]. Initialization/reset requires exclusive
 * ownership; refcount_set() does not publish the object to other users.
 * CONFIG_DEBUG_REFCOUNT diagnoses invalid operations before changing the count.
 * With debugging disabled, invalid operations leave the count unchanged; this
 * prevents arithmetic wraparound but does not repair ownership errors.
 */
static inline void refcount_set(refcount_t *r, unsigned int value)
{
    IFDEF(CONFIG_DEBUG_REFCOUNT,
          Assert(value <= REFCOUNT_MAX,
                 "refcount %p: value %u out of range\n", (void *)r, value));
    if (value > REFCOUNT_MAX)
        return;
    atomic_set(&r->refs, (int32_t)value);
}

/* A snapshot only; reading a nonzero count does not acquire a reference. */
static inline unsigned int refcount_read(const refcount_t *r)
{
    return (unsigned int)atomic_read(&r->refs);
}

/*
 * The caller must keep the object's storage valid throughout this operation.
 * Object visibility must be established by the lookup/publication protocol;
 * taking a reference here provides no acquire ordering or reuse protection.
 * Returns false at zero, or for an invalid count with debugging disabled.
 */
static inline __must_check bool refcount_inc_not_zero(refcount_t *r)
{
    int32_t old = atomic_read(&r->refs);

    do {
        if (!old)
            return false;
        IFDEF(CONFIG_DEBUG_REFCOUNT,
              Assert(old > 0 && old < REFCOUNT_MAX,
                     "refcount %p: increment overflow, count %d\n", (void *)r,
                     (int)old));
        if (old < 0 || old == REFCOUNT_MAX)
            return false;
    } while (!atomic_try_cmpxchg_relaxed(&r->refs, &old, old + 1));
    return true;
}

/* Requires an existing reference or protection that keeps the count nonzero. */
static inline void refcount_inc(refcount_t *r)
{
    bool acquired = refcount_inc_not_zero(r);

    IFDEF(CONFIG_DEBUG_REFCOUNT,
          Assert(acquired, "refcount %p: incrementing a zero count\n",
                 (void *)r));
    (void)acquired;
}

/*
 * Drops a reference with release ordering. The final decrement also provides
 * acquire ordering before the caller destroys the object. The caller must
 * hold a reference and must check the result to handle final destruction.
 * Invalid counts return false with debugging disabled.
 */
static inline __must_check bool refcount_dec_and_test(refcount_t *r)
{
    int32_t old = atomic_read(&r->refs);

    for (;;) {
        IFDEF(CONFIG_DEBUG_REFCOUNT,
              Assert(old > 0, "refcount %p: decrement underflow, count %d\n",
                     (void *)r, (int)old));
        if (old <= 0)
            return false;
        if (old == 1) {
            /* The seq_cst CAS includes the final release/acquire ordering. */
            if (atomic_try_cmpxchg(&r->refs, &old, 0))
                return true;
        } else if (atomic_try_cmpxchg_release(&r->refs, &old, old - 1)) {
            return false;
        }
    }
}

/* Drop a non-final reference; reaching zero requires dec_and_test() instead. */
static inline void refcount_dec(refcount_t *r)
{
    int32_t old = atomic_read(&r->refs);

    do {
        IFDEF(CONFIG_DEBUG_REFCOUNT,
              Assert(old > 0, "refcount %p: decrement underflow, count %d\n",
                     (void *)r, (int)old));
        if (old <= 0)
            return;
        IFDEF(CONFIG_DEBUG_REFCOUNT,
              Assert(old > 1, "refcount %p: dropping the final reference\n",
                     (void *)r));
        if (old == 1)
            return;
    } while (!atomic_try_cmpxchg_release(&r->refs, &old, old - 1));
}

#endif /* _NUVIX_REFCOUNT_H */
