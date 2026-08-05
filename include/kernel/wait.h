#ifndef _CUTEOS_KERNEL_WAIT_H
#define _CUTEOS_KERNEL_WAIT_H

/**
 * @file wait.h
 * @brief Task-owned wait state and source wait-channel primitives.
 */

#include <kernel/compiler.h>
#include <kernel/irq.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>

#define WAIT_OUTCOME_EVENT   1u
#define WAIT_OUTCOME_SIGNAL  2u
#define WAIT_OUTCOME_TIMEOUT 3u

#define WAIT_FLAG_INTERRUPTIBLE 0x01u
#define WAIT_FLAG_KILLABLE	0x02u
#define WAIT_FLAG_MASK		(WAIT_FLAG_INTERRUPTIBLE | WAIT_FLAG_KILLABLE)

#define WAIT_MAX_REGISTRATIONS 64u

typedef uint32_t wait_outcome_t;
typedef uint32_t wait_flags_t;

enum task_wait_policy {
	TASK_WAIT_UNINTERRUPTIBLE,
	TASK_WAIT_INTERRUPTIBLE,
	TASK_WAIT_KILLABLE,
};

enum task_wake_reason {
	TASK_WAKE_NONE,
	TASK_WAKE_EVENT,
	TASK_WAKE_SIGNAL,
	TASK_WAKE_TIMEOUT,
	TASK_WAKE_EXIT,
	TASK_WAKE_CLOSED,
};

enum wait_status {
	WAIT_IDLE,
	WAIT_ACTIVE,
	WAIT_COMPLETING,
};

struct task_struct;
struct task_wait;

struct wait_deadline {
	bool active;
	uint64_t expires;
};

struct wait_channel {
	spinlock_t lock;
	struct list_head waiters;
};

/**
 * @struct wait_entry
 * @brief One registration owned by a task wait object.
 */
struct wait_entry {
	struct list_head channel_node;
	struct list_head registration_node;
	struct task_struct *task;
	struct task_wait *wait;
	struct wait_channel *watched;
	struct wait_channel *channel;
	bool exclusive;
};

/**
 * @struct task_wait
 * @brief Stable synchronous wait state embedded in one task.
 */
struct task_wait {
	spinlock_t lock;
	enum wait_status status;
	enum task_wait_policy policy;
	enum task_wake_reason reason;
	uint64_t generation;
	uint32_t status_value;
	struct list_head registrations;
	struct wait_deadline deadline;
	struct wait_entry entries[WAIT_MAX_REGISTRATIONS];
	uint32_t registration_count;
	struct task_struct *owner;
	struct list_head deadline_node;
	bool deadline_queued;
	uint32_t deadline_cpu;
	uint64_t deadline_generation;
	struct task_struct *deadline_task;
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

#define WAIT_CHANNEL_INIT_RANK(name, rank)                                     \
	{                                                                      \
		.lock = SPINLOCK_INIT(rank),                                   \
		.waiters = LIST_HEAD_INIT((name).waiters),                     \
	}
#define WAIT_CHANNEL_INIT(name)                                                \
	WAIT_CHANNEL_INIT_RANK(name, LOCK_RANK_WAIT_CHANNEL)

__always_inline __must_check __pure
static inline bool wait_context_can_sleep(void)
{
	return in_task_context() && cpu_preempt_count(current_cpu()) == 0 &&
	       !spinlock_held();
}

/** Start one wait generation. The caller must finish it before returning. */
void wait_init(void);

/** Start one wait generation. The caller must finish it before returning. */
__must_check
int wait_start(struct task_wait *wait, wait_flags_t flags, const struct wait_deadline *deadline);

/**
 * Register the current task wait with a source channel. The caller holds the
 * source lock, and keeps it held until this function returns.
 */
__must_check
int wait_prepare(struct task_wait *wait, struct wait_channel *channel, bool exclusive);

/** Block the current task until a source wake, signal, or deadline. */
__must_check
int wait_block(struct task_wait *wait, wait_outcome_t *outcome);

/** Remove all registrations and finish the current wait generation. */
void wait_finish(struct task_wait *wait);

/** Mark an active wait interruptible by a deliverable signal. */
bool wait_wake_signal(struct task_struct *task, bool fatal);

/** Publish a source event for one referenced task wait generation. */
bool wait_wake_event(struct task_struct *task, uint64_t generation);

/** Sleep until an active deadline expires without a source channel. */
__must_check
int wait_sleep_until(const struct wait_deadline *deadline);

/** Publish an exit-request wake for a task being terminated by its proc. */
bool wait_wake_exit(struct task_struct *task);

void wait_expire_deadlines(uint64_t now);
uint64_t wait_next_deadline(uint64_t fallback);

__nonnull(1) __access_no_size(write_only, 1)
void wait_channel_init(struct wait_channel *channel);

__nonnull(1) __access_no_size(read_write, 1)
bool wait_channel_wake_one(struct wait_channel *channel);

__nonnull(1) __access_no_size(read_write, 1)
void wait_channel_wake_all(struct wait_channel *channel);

#endif
