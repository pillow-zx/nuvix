#ifndef _NUVIX_REFCOUNT_H
#define _NUVIX_REFCOUNT_H

#include <nuvix/atomic.h>
#include <nuvix/printk.h>

#define REFCOUNT_MAX INT32_MAX

typedef struct {
	atomic_t refs;
} refcount_t;

#define REFCOUNT_INIT(n) {.refs = ATOMIC_INIT(n)}

static inline void refcount_set(refcount_t *ref, int value)
{
	BUG_ON(!ref);
	BUG_ON(value < 0);
	atomic_set(&ref->refs, value);
}

static inline int refcount_read(const refcount_t *ref)
{
	BUG_ON(!ref);
	return atomic_read(&ref->refs);
}

static inline bool refcount_inc_not_zero(refcount_t *ref)
{
	int old;

	BUG_ON(!ref);
	do {
		old = refcount_read(ref);
		if (old == 0)
			return false;
		BUG_ON(old < 0);
		BUG_ON(old == REFCOUNT_MAX);
	} while (atomic_cmpxchg(&ref->refs, old, old + 1) != old);

	return true;
}

static inline void refcount_inc(refcount_t *ref)
{
	BUG_ON(!refcount_inc_not_zero(ref));
}

static inline void refcount_inc_allow_zero(refcount_t *ref)
{
	int old;

	BUG_ON(!ref);
	do {
		old = refcount_read(ref);
		BUG_ON(old < 0);
		BUG_ON(old == REFCOUNT_MAX);
	} while (atomic_cmpxchg(&ref->refs, old, old + 1) != old);
}

static inline bool refcount_dec_and_test(refcount_t *ref)
{
	int old;

	BUG_ON(!ref);
	do {
		old = refcount_read(ref);
		BUG_ON(old <= 0);
	} while (atomic_cmpxchg(&ref->refs, old, old - 1) != old);

	return old == 1;
}

static inline bool refcount_dec_if_positive(refcount_t *ref)
{
	int old;

	BUG_ON(!ref);
	do {
		old = refcount_read(ref);
		BUG_ON(old < 0);
		if (old == 0)
			return false;
	} while (atomic_cmpxchg(&ref->refs, old, old - 1) != old);

	return old == 1;
}

#endif
