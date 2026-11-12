#ifndef _NUVIX_TICK_H
#define _NUVIX_TICK_H

#include <nuvix/ctypes.h>
#include <asm/time.h>

#define MSEC_PER_SEC    1000UL
#define NSEC_PER_SEC    1000000000UL

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

#define TIMER_ABSTIME   1

struct timespec64 {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/* After tick_init(); preserves downward rounding for existing callers. */
uint64_t time_ticks_from_ms(unsigned int ms);

static __always_inline uint64_t time_read(void)
{
    return arch_read_time();
}

/*
 * After successful tick_init(). Ticks are hardware counts, not 1ms tick events.
 * Tick-to-time conversions round down; time-to-tick conversions round up.
 * Output timespecs are normalized: 0 <= tv_nsec < NSEC_PER_SEC.
 * Fallible helpers return -EOVERFLOW if the result cannot be represented,
 * leaving the output unchanged. Timespec input also returns -EINVAL for
 * negative seconds or a nanosecond field outside the normalized range.
 */
void time_timespec_from_ticks(uint64_t ticks, struct timespec64 *ts);
int time_ns_from_ticks(uint64_t ticks, uint64_t *ns);
int time_ticks_from_timespec(const struct timespec64 *ts, uint64_t *ticks);
int time_ticks_from_ns(uint64_t ns, uint64_t *ticks);

/* Hardware-based monotonic time, without subtracting a boot-time origin. */
void time_get_monotonic(struct timespec64 *ts);
/* Relative nanosecond duration to an absolute deadline, sampled once. */
int time_deadline_from_ns(uint64_t ns, uint64_t *deadline);

/* CPU-local, with current bound and IRQs off. Returns the SBI TIME error. */
long tick_cpu_init(void);
void tick_handle(void);

#endif /* _NUVIX_TICK_H */
