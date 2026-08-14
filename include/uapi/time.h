#ifndef _NUVIX_UAPI_TIME_H
#define _NUVIX_UAPI_TIME_H

/**
 * @file time.h
 * @brief Linux-compatible time and timer UAPI layouts.
 */

/**
 * @typedef clockid_t
 * @brief Linux clock id type used by clock_gettime and related syscalls.
 */
typedef int clockid_t;

/**
 * @typedef timer_t
 * @brief POSIX timer id type returned to userspace by timer_create.
 */
typedef int timer_t;

#define CLOCK_REALTIME		 0
#define CLOCK_MONOTONIC		 1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID	 3
#define CLOCK_MONOTONIC_RAW	 4
#define CLOCK_REALTIME_COARSE	 5
#define CLOCK_MONOTONIC_COARSE	 6
#define CLOCK_BOOTTIME		 7
#define CLOCK_REALTIME_ALARM	 8
#define CLOCK_BOOTTIME_ALARM	 9
#define CLOCK_SGI_CYCLE		 10
#define CLOCK_TAI		 11
#define MAX_CLOCKS		 16

#define TIMER_ABSTIME 0x01

#define UTIME_NOW  0x3fffffff
#define UTIME_OMIT 0x3ffffffe

#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

/**
 * @struct tms
 * @brief CPU tick accounting returned by times().
 *
 * @par Fields
 * - @c tms_utime: User ticks charged to the calling process.
 * - @c tms_stime: Kernel ticks charged to the calling process.
 * - @c tms_cutime: User ticks of waited-for children.
 * - @c tms_cstime: Kernel ticks of waited-for children.
 */
struct tms {
	long tms_utime;
	long tms_stime;
	long tms_cutime;
	long tms_cstime;
};

/**
 * @struct timeval
 * @brief Seconds plus microseconds layout used by gettimeofday.
 *
 * @par Fields
 * - @c tv_sec: Whole seconds.
 * - @c tv_usec: Microseconds within the current second.
 */
struct timeval {
	long tv_sec;
	long tv_usec;
};

/**
 * @struct timezone
 * @brief Obsolete Linux timezone ABI layout.
 *
 * @par Fields
 * - @c tz_minuteswest: Minutes west of UTC.
 * - @c tz_dsttime: Daylight-saving correction type.
 */
struct timezone {
	int tz_minuteswest;
	int tz_dsttime;
};

/**
 * @struct timespec
 * @brief Seconds plus nanoseconds layout used by modern time syscalls.
 *
 * @par Fields
 * - @c tv_sec: Whole seconds.
 * - @c tv_nsec: Nanoseconds within the current second.
 */
struct timespec {
	long tv_sec;
	long tv_nsec;
};

/**
 * @struct itimerspec
 * @brief POSIX timer interval and next-expiration values.
 *
 * @par Fields
 * - @c it_interval: Period for repeating timers.
 * - @c it_value: Next expiration or zero to disarm.
 */
struct itimerspec {
	struct timespec it_interval;
	struct timespec it_value;
};

/**
 * @struct itimerval
 * @brief setitimer/getitimer interval and next-expiration values.
 *
 * @par Fields
 * - @c it_interval: Period for repeating interval timers.
 * - @c it_value: Next expiration or zero to disarm.
 */
struct itimerval {
	struct timeval it_interval;
	struct timeval it_value;
};

#undef offsetof
#define offsetof(t, d) __builtin_offsetof(t, d)

_Static_assert(sizeof(clockid_t) == 4, "clockid_t ABI size mismatch");
_Static_assert(sizeof(timer_t) == 4, "timer_t ABI size mismatch");
_Static_assert(sizeof(struct timespec) == 16, "timespec ABI size mismatch");
_Static_assert(offsetof(struct timespec, tv_nsec) == 8,
	       "timespec tv_nsec ABI offset mismatch");
_Static_assert(sizeof(struct timeval) == 16, "timeval ABI size mismatch");
_Static_assert(offsetof(struct timeval, tv_usec) == 8,
	       "timeval tv_usec ABI offset mismatch");
_Static_assert(sizeof(struct itimerspec) == 32, "itimerspec ABI size mismatch");
_Static_assert(offsetof(struct itimerspec, it_value) == 16,
	       "itimerspec it_value ABI offset mismatch");
_Static_assert(sizeof(struct itimerval) == 32, "itimerval ABI size mismatch");
_Static_assert(offsetof(struct itimerval, it_value) == 16,
	       "itimerval it_value ABI offset mismatch");
_Static_assert(ITIMER_REAL == 0, "ITIMER_REAL ABI value mismatch");
_Static_assert(ITIMER_VIRTUAL == 1, "ITIMER_VIRTUAL ABI value mismatch");
_Static_assert(ITIMER_PROF == 2, "ITIMER_PROF ABI value mismatch");
_Static_assert(CLOCK_PROCESS_CPUTIME_ID == 2,
	       "CLOCK_PROCESS_CPUTIME_ID ABI value mismatch");
_Static_assert(CLOCK_THREAD_CPUTIME_ID == 3,
	       "CLOCK_THREAD_CPUTIME_ID ABI value mismatch");
_Static_assert(CLOCK_TAI == 11, "CLOCK_TAI ABI value mismatch");
_Static_assert(MAX_CLOCKS == 16, "MAX_CLOCKS ABI value mismatch");

#endif
