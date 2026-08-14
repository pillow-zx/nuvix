#ifndef _NUVIX_UAPI_RESOURCE_H
#define _NUVIX_UAPI_RESOURCE_H

/**
 * @file resource.h
 * @brief Linux resource-limit and rusage UAPI layouts.
 */

#include <uapi/time.h>

#define RUSAGE_SELF	0
#define RUSAGE_CHILDREN (-1)
#define RUSAGE_BOTH	(-2)
#define RUSAGE_THREAD	1

#define RLIM_INFINITY (~0UL)

#define RLIMIT_CPU	  0
#define RLIMIT_FSIZE	  1
#define RLIMIT_DATA	  2
#define RLIMIT_STACK	  3
#define RLIMIT_CORE	  4
#define RLIMIT_RSS	  5
#define RLIMIT_NPROC	  6
#define RLIMIT_NOFILE	  7
#define RLIMIT_MEMLOCK	  8
#define RLIMIT_AS	  9
#define RLIMIT_LOCKS	  10
#define RLIMIT_SIGPENDING 11
#define RLIMIT_MSGQUEUE	  12
#define RLIMIT_NICE	  13
#define RLIMIT_RTPRIO	  14
#define RLIMIT_RTTIME	  15
#define RLIM_NLIMITS	  16

/**
 * @struct rlimit64
 * @brief Soft and hard resource limits used by prlimit64.
 *
 * @par Fields
 * - @c rlim_cur: Current soft limit.
 * - @c rlim_max: Maximum hard limit.
 */
struct rlimit64 {
	unsigned long rlim_cur;
	unsigned long rlim_max;
};

/**
 * @struct rusage
 * @brief Linux getrusage result layout.
 *
 * @par Fields
 * - @c ru_utime: User CPU time.
 * - @c ru_stime: Kernel CPU time.
 * - @c ru_maxrss: Maximum resident set size.
 * - @c ru_ixrss: Integral shared text memory size.
 * - @c ru_idrss: Integral unshared data size.
 * - @c ru_isrss: Integral unshared stack size.
 * - @c ru_minflt: Page reclaims without I/O.
 * - @c ru_majflt: Page faults requiring I/O.
 * - @c ru_nswap: Swap operations.
 * - @c ru_inblock: Block input operations.
 * - @c ru_oublock: Block output operations.
 * - @c ru_msgsnd: IPC messages sent.
 * - @c ru_msgrcv: IPC messages received.
 * - @c ru_nsignals: Signals received.
 * - @c ru_nvcsw: Voluntary context switches.
 * - @c ru_nivcsw: Involuntary context switches.
 */
struct rusage {
	struct timeval ru_utime;
	struct timeval ru_stime;
	long ru_maxrss;
	long ru_ixrss;
	long ru_idrss;
	long ru_isrss;
	long ru_minflt;
	long ru_majflt;
	long ru_nswap;
	long ru_inblock;
	long ru_oublock;
	long ru_msgsnd;
	long ru_msgrcv;
	long ru_nsignals;
	long ru_nvcsw;
	long ru_nivcsw;
};

#undef offsetof
#define offsetof(t, d) __builtin_offsetof(t, d)

_Static_assert(sizeof(struct rlimit64) == 16, "rlimit64 ABI size mismatch");
_Static_assert(sizeof(struct rusage) == 144, "rusage ABI size mismatch");
_Static_assert(offsetof(struct rusage, ru_stime) == 16,
	       "rusage ru_stime ABI offset mismatch");
_Static_assert(offsetof(struct rusage, ru_nivcsw) == 136,
	       "rusage ru_nivcsw ABI offset mismatch");

#endif
