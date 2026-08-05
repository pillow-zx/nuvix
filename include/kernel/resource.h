#ifndef _CUTEOS_KERNEL_RESOURCE_H
#define _CUTEOS_KERNEL_RESOURCE_H

/**
 * @file resource.h
 * @brief Resource limits and CPU-time accounting helpers.
 */

#include <kernel/compiler.h>
#include <kernel/cputime.h>
#include <kernel/timer.h>
#include <uapi/resource.h>

/**
 * @def CPUTIME_USEC_PER_SEC
 * @brief Microseconds per second for rusage timeval conversion.
 */
#define CPUTIME_USEC_PER_SEC 1000000ULL

static_assert(sizeof(struct rusage) == 144,
	      "struct rusage must match the riscv64 ABI (144 bytes)");
static_assert(offsetof(struct rusage, ru_stime) == 16,
	      "ru_stime offset drifted from riscv64 rusage ABI");
static_assert(offsetof(struct rusage, ru_nivcsw) == 136,
	      "ru_nivcsw offset drifted from riscv64 rusage ABI");

/**
 * @brief Initialize default resource limits for a new proc_struct.
 * @param rlimits RLIM_NLIMITS-sized table to initialize.
 */
void rlimits_init(struct rlimit64 rlimits[RLIM_NLIMITS]);

/**
 * @brief Convert scheduler ticks to a Linux timeval.
 * @param ticks CPU time measured in scheduler ticks.
 * @param tv Output timeval.
 */
__nonnull(2) __access_no_size(write_only, 2)
static inline void cputime_timeval(const uint64_t ticks, struct timeval *tv)
{
	uint64_t sec = ticks / HZ;
	uint64_t rem = ticks % HZ;

	tv->tv_sec = (long)sec;
	tv->tv_usec = (long)(rem * CPUTIME_USEC_PER_SEC / HZ);
}

/**
 * @brief Convert task CPU tick counters into a Linux rusage record.
 * @param time User/system tick counters.
 * @param ru Output rusage with CPU fields populated.
 */
__nonnull(1, 2) __access_no_size(read_only, 1) __access_no_size(write_only, 2)
static inline void cputime_rusage(const struct task_cputime *time, struct rusage *ru)
{
	struct timeval utime;
	struct timeval stime;

	cputime_timeval(time->utime_ticks, &utime);
	cputime_timeval(time->stime_ticks, &stime);

	*ru = (struct rusage){
		.ru_utime = utime,
		.ru_stime = stime,
	};
}

#endif
