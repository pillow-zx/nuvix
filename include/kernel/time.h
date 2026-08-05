#ifndef _CUTEOS_KERNEL_TIME_H
#define _CUTEOS_KERNEL_TIME_H

/**
 * @file time.h
 * @brief Kernel timer, itimer, POSIX timer, and mtime conversion APIs.
 */

#include <kernel/compiler.h>
#include <kernel/list.h>
#include <kernel/mutex.h>
#include <kernel/refcount.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>
#include <uapi/signal.h>
#include <uapi/time.h>

struct ktimer;
struct signal_struct;
struct task_struct;
struct wait_deadline;

/**
 * @typedef ktimer_fn_t
 * @brief Callback invoked when a kernel timer expires.
 */
typedef void (*ktimer_fn_t)(struct ktimer *timer, void *arg);

/**
 * @struct ktimer
 * @brief Kernel timer driven by the architecture mtime clock.
 *
 * @par Fields
 * - @c node: Node in the active timer list.
 * - @c function: Expiration callback.
 * - @c arg: Opaque callback argument.
 * - @c expires: Absolute mtime tick when the timer fires.
 * - @c interval: Repeat interval in mtime ticks, or 0.
 * - @c active: True while linked into timer state.
 * - @c callback_running: Callback is executing outside the timer queue lock.
 * - @c callback_task: Task context that invoked the callback, when known.
 * - @c cancel_waiters: Tasks waiting for callback completion.
 */
struct ktimer {
	struct list_head node;
	ktimer_fn_t function;
	void *arg;
	uint64_t expires;
	uint64_t interval;
	bool active;
	bool callback_running;
	struct task_struct *callback_task;
	struct list_head cancel_waiters;
};

/**
 * @def ITIMER_COUNT
 * @brief Number of Linux interval timer slots per signal_struct.
 */
constexpr uint32_t ITIMER_COUNT = 3;

/**
 * @struct itimer_state
 * @brief Per-thread-group state for setitimer/getitimer.
 *
 * @par Fields
 * - @c operation_lock: Serializes operations that may synchronously cancel the
 *   backing timer.
 * - @c lock: Protects value and timer state.
 * - @c value: Current Linux itimerval.
 * - @c timer: Backing kernel timer.
 * - @c target: Task receiving SIGALRM-style delivery.
 * - @c target_ref: Target reference held while the target is installed.
 */
struct itimer_state {
	mutex_t operation_lock;
	spinlock_t lock;
	struct itimerval value;
	struct ktimer timer;
	struct task_struct *target;
	bool target_ref;
};

/**
 * @def POSIX_TIMER_COUNT
 * @brief Maximum number of POSIX timers per signal_struct.
 */
constexpr int32_t POSIX_TIMER_COUNT = 32;

/**
 * @struct posix_timer
 * @brief One POSIX timer created by timer_create.
 *
 * @par Fields
 * - @c timer: Backing kernel timer.
 * - @c refs: Table ownership and in-flight operation references.
 * - @c signal: Owning thread-group signal state.
 * - @c target: Target task for notification delivery.
 * - @c value: Current Linux timer value.
 * - @c sigev_value: sigevent payload.
 * - @c clock_id: CLOCK_* id selected at creation.
 * - @c id: Userspace-visible timer id.
 * - @c signo: Signal number for SIGEV_SIGNAL-like modes.
 * - @c notify: SIGEV_* notification mode.
 * - @c overrun: Overrun count reported by timer_getoverrun.
 * - @c allocated: Slot is owned by userspace.
 * - @c target_ref: Target reference held while the slot is allocated.
 */
struct posix_timer {
	struct ktimer timer;
	refcount_t refs;
	struct signal_struct *signal;
	struct task_struct *target;
	struct itimerspec value;
	sigval_t sigev_value;
	clockid_t clock_id;
	timer_t id;
	int signo;
	int notify;
	int overrun;
	bool allocated;
	bool target_ref;
};

/**
 * @struct posix_timer_table
 * @brief Fixed-size POSIX timer table owned by signal_struct.
 *
 * @par Fields
 * - @c lock: Protects allocation bitmap and slots.
 * - @c allocated: Bitset of allocated timer ids.
 * - @c timers: Timer slots.
 */
struct posix_timer_table {
	spinlock_t lock;
	unsigned long allocated;
	struct posix_timer *timers[POSIX_TIMER_COUNT];
};

__must_check __pure
static inline bool clock_id_supported(int clock_id)
{
	return clock_id == CLOCK_REALTIME || clock_id == CLOCK_MONOTONIC ||
	       clock_id == CLOCK_BOOTTIME;
}

__must_check __pure
static inline bool itimer_which_valid(int which)
{
	return which == ITIMER_REAL || which == ITIMER_VIRTUAL ||
	       which == ITIMER_PROF;
}

__must_check __pure
static inline size_t itimer_which_index(int which)
{
	return (size_t)which;
}

__must_check __pure
static inline bool posix_timer_id_valid(timer_t id)
{
	return id >= 0 && id < POSIX_TIMER_COUNT;
}

__nonnull(1, 2) __access_no_size(read_only, 1) __access_no_size(write_only, 2)
static inline void itimer_state_value( const struct itimer_state *state,
                struct itimerval *value)
{
	*value = state->value;
}

__must_check __pure __nonnull(1) __access_no_size(read_only,1)
static inline bool ktimer_active(const struct ktimer *timer)
{
	return timer->active;
}

__must_check __pure __nonnull(1) __access_no_size(read_only,1)
static inline bool ktimer_expired(const struct ktimer *timer, uint64_t now)
{
	return timer->active && timer->expires <= now;
}

__must_check __pure __nonnull(1) __access_no_size(read_only, 1)
static inline uint64_t ktimer_remaining(const struct ktimer *timer, uint64_t now)
{
	if (!timer->active || timer->expires <= now)
		return 0;
	return timer->expires - now;
}

/**
 * @brief Convert architecture mtime ticks to a Linux timespec.
 * @param ticks mtime tick count.
 * @param ts Output timespec.
 */
__nonnull(2) __access_no_size(write_only, 2)
void mtime_to_timespec(uint64_t ticks, struct timespec *ts);

/**
 * @brief Read the runtime CLOCK_REALTIME value.
 *
 * The value is architecture mtime plus a volatile wall-clock offset. The
 * offset is reset at boot because cuteOS has no RTC persistence source.
 *
 * @param value Output Linux timespec.
 */
__nonnull(1) __access_no_size(write_only, 1)
void kernel_realtime_now(struct timespec *value);

/**
 * @brief Set the runtime CLOCK_REALTIME value.
 *
 * The value must be a normalized, nonnegative timespec and cannot precede
 * the current CLOCK_MONOTONIC value.
 *
 * @param value Requested Linux timespec.
 * @return 0 on success, or a negative errno.
 */
 __must_check __nonnull(1) __access_no_size(read_only, 1)
int kernel_realtime_set(const struct timespec *value);

/**
 * @brief Convert a relative Linux timespec to mtime ticks.
 * @param ts Input relative timespec.
 * @param delta Output tick delta.
 * @return 0 on success, or a negative errno.
 */
__must_check __access_no_size(read_only, 1) __access_no_size(write_only, 2)
int timespec_to_mtime_delta(const struct timespec *ts, uint64_t *delta);

/**
 * @brief Add a tick delta to a current mtime value with saturation handling.
 * @param now Current mtime.
 * @param delta Relative tick delta.
 * @return Absolute deadline.
 */
__must_check __const
uint64_t mtime_deadline_after(uint64_t now, uint64_t delta);

__must_check __nonnull(2) __access_no_size(write_only, 2)
int mtime_deadline_from_timespec(const struct timespec *ts,
                struct wait_deadline *deadline);

__must_check __nonnull(2) __access_no_size(write_only, 2)
int mtime_deadline_from_ms(long timeout_ms, struct wait_deadline *deadline);

/**
 * @brief Initialize a kernel timer object.
 * @param timer Timer to initialize.
 * @param function Expiration callback.
 * @param arg Opaque callback argument.
 */
__nonnull(1) __access_no_size(write_only, 1)
void ktimer_init(struct ktimer *timer, ktimer_fn_t function, void *arg);

/**
 * @brief Arm or rearm a kernel timer.
 * @param timer Timer to arm.
 * @param expires Absolute mtime expiration.
 * @param interval Repeat interval, or 0 for one-shot.
 * @return 0 on success, or a negative errno.
 */
__must_check __nonnull(1) __access_no_size(read_write, 1)
int ktimer_arm(struct ktimer *timer, uint64_t expires, uint64_t interval);

/**
 * @brief Cancel an active kernel timer.
 * @param timer Timer to cancel.
 * @return true if the timer was active.
 */
__must_check __nonnull(1) __access_no_size(read_write, 1)
bool ktimer_cancel(struct ktimer *timer);

/**
 * @brief Cancel a kernel timer and wait for an in-flight callback to finish.
 *
 * This task-context operation may be called with local IRQs enabled or
 * disabled, but not from hard IRQ context, while holding a spinlock, or with
 * preemption disabled. It preserves the caller's IRQ state. A callback cannot
 * synchronously cancel itself.
 *
 * @return 0 on success, or a negative errno for an invalid context or
 *         callback self-cancellation.
 */
__must_check __nonnull(1) __access_no_size(read_write, 1)
int ktimer_cancel_sync(struct ktimer *timer);

void ktimer_run_expired(uint64_t now);

__nonnull(1) __access_no_size(read_write, 1)
void itimer_state_init(struct itimer_state *state);

__nonnull(1) __access_no_size(read_write, 1)
void itimer_state_destroy(struct itimer_state *state);

__must_check __nonnull(1, 2)
__access_no_size(read_write, 1) __access_no_size(read_only, 2)
int itimer_get_value(struct itimer_state *state, struct itimerval *value);

__must_check __nonnull(1, 2, 3)
__access_no_size(read_write, 1) __access_no_size(read_only, 3)
int itimer_set_real(struct itimer_state *state, struct task_struct *target,
		const struct itimerval *new_value, struct itimerval *old_value);

__nonnull(1) __access_no_size(read_write, 1)
void posix_timer_table_init(struct posix_timer_table *table);

__nonnull(1) __access_no_size(read_write, 1)
void posix_timer_table_clear(struct posix_timer_table *table);

__nonnull(1) __access_no_size(read_write, 1)
void posix_timer_table_destroy(struct posix_timer_table *table);

__must_check __nonnull(1, 3)
__access_no_size(read_write, 1) __access_no_size(write_only, 3)
int posix_timer_create(struct signal_struct *signal, clockid_t clock_id,
                timer_t *timerid, const sigevent_t *event, struct task_struct *target);

__must_check __nonnull(1, 3)
__access_no_size(read_write, 1) __access_no_size(write_only, 3)
int posix_timer_gettime(struct signal_struct *signal, timer_t id,
                struct itimerspec *value);

__must_check __nonnull(1, 4)
__access_no_size(read_write, 1) __access_no_size(read_only, 4)
int posix_timer_settime(struct signal_struct *signal, timer_t id, int flags,
                const struct itimerspec *new_value, struct itimerspec *old_value);

__must_check __nonnull(1) __access_no_size(read_write, 1)
int  posix_timer_getoverrun(struct signal_struct *signal, timer_t id);

__must_check __nonnull(1) __access_no_size(read_write, 1)
int posix_timer_delete(struct signal_struct *signal, timer_t id);

#endif
