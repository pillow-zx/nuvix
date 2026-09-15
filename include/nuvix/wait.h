#ifndef _NUVIX_WAIT_H
#define _NUVIX_WAIT_H

#include <nuvix/compiler.h>
#include <nuvix/irq.h>
#include <nuvix/list.h>
#include <nuvix/spinlock.h>
#include <nuvix/types.h>

#define WAIT_OUTCOME_EVENT 1u
#define WAIT_OUTCOME_SIGNAL 2u
#define WAIT_OUTCOME_TIMEOUT 3u
#define WAIT_FLAG_INTERRUPTIBLE 0x01u
#define WAIT_FLAG_KILLABLE 0x02u
#define WAIT_FLAG_MASK (WAIT_FLAG_INTERRUPTIBLE | WAIT_FLAG_KILLABLE)

typedef uint32_t wait_outcome_t;
typedef uint32_t wait_flags_t;

enum task_wait_policy {
	TASK_WAIT_UNINTERRUPTIBLE,
	TASK_WAIT_INTERRUPTIBLE,
	TASK_WAIT_KILLABLE,
};
enum task_wait_signal_mode {
	TASK_WAIT_SIGNAL_DEFAULT,
	TASK_WAIT_SIGNAL_SET,
};
enum wait_phase {
	WAIT_IDLE,
	WAIT_ARMED,
	WAIT_BLOCKED,
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
struct wait_entry {
	struct list_head channel_node;
	struct task_struct *task;
	struct wait_channel *channel;
	uint64_t generation;
	bool exclusive;
};
struct wait_scope {
	struct task_wait *wait;
	uint64_t generation;
	bool active;
	struct wait_entry single;
	/* Optional storage prepared by a multi-source caller before begin. */
	struct wait_entry *entries;
	size_t capacity;
};
struct task_wait {
	enum wait_phase phase;
	enum task_wait_policy policy;
	enum task_wait_signal_mode signal_mode;
	uint64_t signal_set;
	uint64_t generation;
	uint32_t status_value;
	struct wait_deadline deadline;
	struct wait_entry *entries;
	size_t capacity;
	size_t registration_count;
	struct task_struct *owner;
	struct list_head deadline_node;
	bool deadline_queued;
	uint32_t deadline_cpu;
	uint64_t deadline_generation;
	struct task_struct *deadline_task;
};

static inline struct wait_deadline wait_deadline_none(void)
{
	return (struct wait_deadline){0};
}
static inline struct wait_deadline wait_deadline_at(uint64_t expires)
{
	return (struct wait_deadline){.active = true, .expires = expires};
}

#define WAIT_CHANNEL_INIT_RANK(name, rank, irq_policy) \
	{.lock = SPINLOCK_INIT(rank, irq_policy), \
	 .waiters = LIST_HEAD_INIT((name).waiters)}
#define WAIT_CHANNEL_INIT(name) \
	WAIT_CHANNEL_INIT_RANK(name, LOCK_RANK_WAIT_CHANNEL, LOCK_IRQ_TASK_ONLY)

static inline bool wait_context_can_sleep(void)
{
	return in_task_context() && !irqs_disabled() &&
	       cpu_preempt_count(current_cpu()) == 0 && !spinlock_held();
}
bool wait_may_block(void);
void wait_init(void);
/* No faulting access, allocation, or nested sleeping lock until complete.
 * Predicate check and registration must share the predicate owner's lock. */
int wait_scope_begin(struct wait_scope *scope, wait_flags_t flags,
		     const struct wait_deadline *deadline);
int wait_scope_begin_signal_set(struct wait_scope *scope, wait_flags_t flags,
				const struct wait_deadline *deadline,
				uint64_t signal_set);
int wait_scope_begin_signal_set_locked(struct wait_scope *scope,
				      wait_flags_t flags,
				      const struct wait_deadline *deadline,
				      uint64_t signal_set);
int wait_scope_prepare(struct wait_scope *scope, struct wait_channel *channel,
		       bool exclusive);
int wait_scope_prepare_current(struct wait_channel *channel, bool exclusive);
int wait_scope_block(struct wait_scope *scope, wait_outcome_t *outcome);
void wait_scope_complete(struct wait_scope *scope);
void wait_scope_cleanup(struct wait_scope *scope);
void wait_cancel_current(void);
#define __wait_scope __cleanup(wait_scope_cleanup)

bool wait_wake_signal(struct task_struct *task, bool fatal);
bool wait_wake_event(struct task_struct *task, uint64_t generation);
bool wait_wake_exit(struct task_struct *task);
int wait_sleep_until(const struct wait_deadline *deadline);
void wait_expire_deadlines(uint64_t now);
uint64_t wait_next_deadline(uint64_t fallback);
void wait_channel_init(struct wait_channel *channel);
bool wait_channel_wake_one(struct wait_channel *channel);
void wait_channel_wake_all(struct wait_channel *channel);
#endif
