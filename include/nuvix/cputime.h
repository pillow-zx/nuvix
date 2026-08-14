#ifndef _NUVIX_CPUTIME_H
#define _NUVIX_CPUTIME_H

/**
 * @file cputime.h
 * @brief CPU-time counters and overflow-safe arithmetic.
 *
 * Counters are kept in unsigned scheduler ticks.  Accounting is monotonic:
 * an exhausted counter remains at UINT64_MAX instead of wrapping to zero.
 * Linux riscv64 exposes clock_t as a signed 64-bit long, so ABI exports clamp
 * values above INT64_MAX rather than producing a negative result.
 */

#include <nuvix/types.h>

struct task_cputime {
	uint64_t utime_ticks;
	uint64_t stime_ticks;
};

static inline uint64_t cputime_saturating_add(uint64_t left, uint64_t right)
{
	return left > UINT64_MAX - right ? UINT64_MAX : left + right;
}

static inline void cputime_add(struct task_cputime *dst,
			       const struct task_cputime *src)
{
	if (!dst || !src)
		return;
	dst->utime_ticks =
		cputime_saturating_add(dst->utime_ticks, src->utime_ticks);
	dst->stime_ticks =
		cputime_saturating_add(dst->stime_ticks, src->stime_ticks);
}

static inline int64_t cputime_to_clock_t(uint64_t ticks)
{
	return ticks > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)ticks;
}

#endif
