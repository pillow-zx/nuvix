#ifndef _NUVIX_TIME_H
#define _NUVIX_TIME_H

/**
 * @file time.h
 * @brief Clock conversion and realtime-offset interfaces.
 *
 * Kernel timeout ownership lives in the task wait module. This interface
 * deliberately contains no callback timer or interval-timer state.
 */

#include <nuvix/compiler.h>
#include <nuvix/types.h>
#include <uapi/time.h>

struct wait_deadline;

__nonnull(2) __access_no_size(write_only, 2)
void mtime_to_timespec(uint64_t ticks, struct timespec *ts);

__nonnull(1) __access_no_size(write_only, 1)
void kernel_realtime_now(struct timespec *value);

__must_check __nonnull(1) __access_no_size(read_only, 1)
int kernel_realtime_set(const struct timespec *value);

__must_check __access_no_size(read_only, 1)
__access_no_size(write_only, 2)
int timespec_to_mtime_delta(const struct timespec *ts, uint64_t *delta);

__must_check __const
uint64_t mtime_deadline_after(uint64_t now, uint64_t delta);

/** Convert a relative timespec, or disable the deadline when @p ts is NULL. */
__must_check __nonnull(2) __access_no_size(write_only, 2)
int mtime_deadline_from_timespec(const struct timespec *ts,
				 struct wait_deadline *deadline);

/** Convert a relative millisecond timeout; negative values mean infinite. */
__must_check __nonnull(2) __access_no_size(write_only, 2)
int mtime_deadline_from_ms(long timeout_ms,
				   struct wait_deadline *deadline);

#endif
