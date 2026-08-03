#ifndef _CUTEOS_KERNEL_WAIT_H
#define _CUTEOS_KERNEL_WAIT_H

/**
 * @file wait.h
 * @brief Conditional waiting primitives used by blocking kernel paths.
 */

#include <kernel/list.h>
#include <kernel/compiler.h>
#include <kernel/irq.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>

constexpr uint32_t WAIT_OUTCOME_EVENT = 1u;
constexpr uint32_t WAIT_OUTCOME_SIGNAL = 2u;
constexpr uint32_t WAIT_OUTCOME_TIMEOUT = 3u;

constexpr uint32_t WAIT_FLAG_INTERRUPTIBLE = 0x01u;
constexpr uint32_t WAIT_FLAG_KILLABLE = 0x02u;
constexpr uint32_t WAIT_FLAG_MASK =
	WAIT_FLAG_INTERRUPTIBLE | WAIT_FLAG_KILLABLE;

constexpr uint32_t WAIT_SESSION_MAX_CHANNELS = 64u;

typedef uint32_t wait_outcome_t;
typedef uint32_t wait_flags_t;

enum wait_kind {
	WAIT_KIND_GENERIC = 0,
	WAIT_KIND_MUTEX,
	WAIT_KIND_FUTEX,
	WAIT_KIND_PIPE,
	WAIT_KIND_POLL,
	WAIT_KIND_CHILD,
};

struct task_struct;
struct wait_session;
struct wait_channel;

struct wait_deadline {
	bool active;
	uint64_t expires;
};

/**
 * @brief Test whether the current context may enter wait_for().
 *
 * Waiting requires non-idle task context, a preemptible CPU, no hard-IRQ
 * handler, and no held spinlock. IRQ-off task context is allowed; wait_for()
 * handles the IRQ-enabled handoff before WFI. This read-only guard does not
 * change IRQ state, preempt_count, or task state.
 * wait_for() diagnoses a false result with BUG_ON before touching its inputs.
 */
__always_inline __must_check __pure
static inline bool wait_context_can_sleep(void)
{
	return in_task_context() && cpu_preempt_count(current_cpu()) == 0 &&
	       !spinlock_held();
}

/**
 * @brief Inspect or claim an event and register its wait channels.
 *
 * The check must inspect or claim its event while holding the source lock and
 * call wait_session_watch() before releasing that lock. The lock order is
 * source lock followed by wait_channel::lock. A positive return reports an
 * available event, zero reports no event, and a negative return reports an
 * operation error. Edge events must be latched in source-owned state before
 * waking a waiter.
 */
typedef int (*wait_check_fn)(struct wait_session *context, void *arg);

/**
 * @brief Cancel adapter-owned state for a task being torn down.
 * @param arg Adapter argument originally supplied by wait_request.
 *
 * The wait core has already removed task-owned channel registrations before
 * invoking this callback. The adapter must release any state that is not
 * represented by a wait entry, such as a secondary waiter list.
 */
typedef void (*wait_cancel_fn)(void *arg);

/**
 * @struct wait_request
 * @brief Adapter-owned condition observed by wait_for().
 *
 * The request, its check argument, watched channels, and their owning objects
 * must remain alive until wait_for() returns. channel_limit is the maximum
 * number of distinct channels watched during the invocation.
 */
struct wait_request {
	enum wait_kind kind;
	wait_check_fn check;
	wait_cancel_fn cancel;
	void *arg;
	uint32_t channel_limit;
};

__always_inline __must_check __const
static inline struct wait_deadline wait_deadline_none(void)
{
	return (struct wait_deadline){.active = false, .expires = 0};
}

__always_inline __must_check __const
static inline struct wait_deadline wait_deadline_at(uint64_t expires)
{
	return (struct wait_deadline){.active = true, .expires = expires};
}

/**
 * @struct wait_channel
 * @brief Channel on which tasks wait for a possible condition change.
 *
 * @par Fields
 * - @c lock: Protects waiters.
 * - @c waiters: Tasks waiting for a possible condition change.
 */
struct wait_channel {
	spinlock_t lock;
	struct list_head waiters;
};

/**
 * @def WAIT_CHANNEL_INIT
 * @brief Static initializer for an empty wait channel.
 */
#define WAIT_CHANNEL_INIT(name)                                                \
	{                                                                      \
		.lock = SPINLOCK_INIT,                                         \
		.waiters = LIST_HEAD_INIT((name).waiters),                     \
	}

__must_check __access_no_size(read_write, 1) __access_no_size(read_write, 2)
int wait_session_watch(struct wait_session *session, struct wait_channel *channel);

/**
 * @brief Wait for an event, signal, or deadline.
 *
 * The caller must be in non-idle task context, outside hard-IRQ context, with
 * preemption enabled and no held spinlock. The function may block and may
 * allocate wait-session state; it preserves the caller's local IRQ state on
 * return. It acquires wait-channel locks only through the request callback and
 * owns no caller lock.
 * The request, deadline, and outcome storage must remain valid until return.
 * Outcome priority is EVENT, then SIGNAL, then TIMEOUT.
 * `WAIT_FLAG_INTERRUPTIBLE` reports an unblocked pending signal;
 * `WAIT_FLAG_KILLABLE` reports only a pending `SIGKILL`. Wakeups that do not
 * make any outcome true are retried internally. On every return the current
 * task is running and all channel watches and timeout state are cleaned. A
 * negative return is an operation error and leaves outcome set to zero.
 * Production callers should use the fixed-policy wrappers below; direct use
 * is reserved for the wait implementation and white-box tests.
 * Calling this function when wait_context_can_sleep() is false is a kernel
 * context violation and triggers BUG_ON.
 */
__must_check __access_no_size(write_only, 4)
__access_no_size(read_only, 1) __access_no_size(read_only, 3)
int wait_for(const struct wait_request *request, wait_flags_t flags,
	     const struct wait_deadline *deadline,
	     wait_outcome_t *outcome);

/**
 * @brief Wait with one fixed signal-sleep policy.
 *
 * These wrappers preserve wait_for()'s core error and outcome contract while
 * preventing callers from combining incompatible wait flags.
 */
__always_inline __must_check __access_no_size(write_only, 3)
__access_no_size(read_only, 1) __access_no_size(read_only, 2)
static inline int wait_for_interruptible(const struct wait_request *request,
				 const struct wait_deadline *deadline,
				 wait_outcome_t *outcome)
{
	return wait_for(request, WAIT_FLAG_INTERRUPTIBLE, deadline, outcome);
}

__always_inline __must_check __access_no_size(write_only, 3)
__access_no_size(read_only, 1) __access_no_size(read_only, 2)
static inline int wait_for_killable(const struct wait_request *request,
				const struct wait_deadline *deadline,
				wait_outcome_t *outcome)
{
	return wait_for(request, WAIT_FLAG_KILLABLE, deadline, outcome);
}

__always_inline __must_check __access_no_size(write_only, 3)
__access_no_size(read_only, 1) __access_no_size(read_only, 2)
static inline int wait_for_uninterruptible(const struct wait_request *request,
				    const struct wait_deadline *deadline,
				    wait_outcome_t *outcome)
{
	return wait_for(request, 0, deadline, outcome);
}

/**
 * @brief Wait for a request to produce an event without signal interruption.
 *
 * This is the no-deadline form used by mutex-like paths. A successful return
 * means the request produced WAIT_OUTCOME_EVENT.
 */
__must_check __access_no_size(read_only, 1)
int wait_event_uninterruptible(const struct wait_request *request);

/**
 * @brief Sleep until an active deadline expires without a request or signal.
 * @return zero after WAIT_OUTCOME_TIMEOUT, or a negative operation error.
 */
__must_check __access_no_size(read_only, 1)
int wait_sleep_until(const struct wait_deadline *deadline);

void wait_cancel_task(struct task_struct *task);

__nonnull(1) __access_no_size(write_only, 1)
void wait_channel_init(struct wait_channel *channel);

/**
 * @brief Detach and wake one sleeping waiter, if present.
 * @return true when a waiter was detached and passed to the scheduler.
 *
 * Only the channel lock is held while the waiter is removed. The wait entry
 * keeps its task reference until the owning wait_for() cleanup. The wake path
 * takes an additional task reference while it invokes the scheduler outside
 * the channel lock.
 */
__nonnull(1) __access_no_size(read_write, 1)
bool wait_channel_wake_one(struct wait_channel *channel);

__nonnull(1) __access_no_size(read_write, 1)
void wait_channel_wake_all(struct wait_channel *channel);

#endif
