/*
 * kernel/waitqueue.c - task-owned conditional waits
 *
 * A wait is a synchronous operation of the current task.  The source owns
 * its condition and publishes a wait entry while holding the source lock;
 * this module only owns the generation handshake and task wakeup.
 */

#include <nuvix/errno.h>
#include <nuvix/irq.h>
#include <nuvix/printk.h>
#include <nuvix/processor.h>
#include <nuvix/sched.h>
#include <nuvix/signal.h>
#include <nuvix/task.h>
#include <nuvix/timer.h>
#include <nuvix/tools.h>
#include <nuvix/wait.h>

struct wait_deadline_queue {
	spinlock_t lock;
	struct list_head entries;
};

static struct wait_deadline_queue deadline_queues[NR_CPUS];

#ifdef CONFIG_DEBUG_CONTEXT
#define WAIT_DENIAL_REPORT_TICKS (MTIME_FREQ / 10)
#endif

void wait_init(void)
{
	irq_flags_t flags = local_irq_save();

	for (uint32_t id = 0; id < NR_CPUS; id++) {
		spin_lock_init(&deadline_queues[id].lock,
					LOCK_RANK_DEADLINE_QUEUE,
					LOCK_IRQ_HARDIRQ_REACHABLE);
		INIT_LIST_HEAD(&deadline_queues[id].entries);
	}
	local_irq_restore(flags);
}

static bool wait_active(const struct task_wait *wait)
{
	return wait->phase == WAIT_ARMED || wait->phase == WAIT_BLOCKED;
}

#ifdef CONFIG_DEBUG_CONTEXT
static void wait_denial_reset_locked(struct task_wait *wait)
{
	wait->denial_started = 0;
	wait->denial_generation = 0;
	wait->denial_retries = 0;
	wait->denial_reported = false;
}

static void wait_denial_observe(struct task_struct *task,
				enum sched_park_result result)
{
	struct task_wait *wait = &task->wait;
	uint64_t generation = 0;
	uint64_t duration = 0;
	uint64_t now;
	uint32_t retries = 0;
	enum task_lifecycle lifecycle = TASK_NEW;
	enum task_run_state run_state = TASK_STOPPED;
	enum wait_phase phase = WAIT_IDLE;
	uint32_t cpu_id = UINT32_MAX;
	bool on_rq = false;
	bool on_cpu = false;
	bool report = false;
	irq_flags_t flags;

	spin_lock_irqsave(&wait->lock, &flags);
	if (result != SCHED_PARK_STATE_DENIED) {
		wait_denial_reset_locked(wait);
		spin_unlock_irqrestore(&wait->lock, flags);
		return;
	}

	now = timer_now();

	BUG_ON(!wait->denial_retries ||
	       wait->denial_generation != wait->generation);
	if (!wait->denial_reported &&
	    now - wait->denial_started >= WAIT_DENIAL_REPORT_TICKS) {
		wait->denial_reported = true;
		report = true;
		generation = wait->denial_generation;
		duration = now - wait->denial_started;
		retries = wait->denial_retries;
		lifecycle = task->lifecycle;
		run_state = task->run_state;
		phase = wait->phase;
		cpu_id = task->cpu ? task->cpu->id : UINT32_MAX;
		on_rq = task->on_rq;
		on_cpu = task->on_cpu;
	}
	spin_unlock_irqrestore(&wait->lock, flags);

	if (report)
		pr_warn("wait: park denial task=%p lifecycle=%u run_state=%u "
			"cpu=%u on_rq=%u on_cpu=%u phase=%u generation=%lu "
			"duration=%lu retries=%u\n",
			(void *)task, lifecycle, run_state, cpu_id, on_rq, on_cpu,
			phase, (size_t)generation, (size_t)duration, retries);
}
#else
static void wait_denial_observe(struct task_struct *task,
				enum sched_park_result result)
{
	(void)task;
	(void)result;
}
#endif

bool wait_may_block(void)
{
	struct task_struct *task = current_task();

	return task && wait_context_can_sleep() &&
	       task->wait.phase == WAIT_IDLE;
}

static void wait_deadline_remove(struct task_wait *wait)
{
	struct wait_deadline_queue *queue;
	struct task_struct *task = NULL;
	irq_flags_t flags;

	/* Deadline ownership stays with the CPU that armed it. Task migration does
	 * not move the timer entry: that online CPU expires the saved generation
	 * and sched_wake() performs any required remote placement. */
	queue = &deadline_queues[wait->deadline_cpu];
	spin_lock_irqsave(&queue->lock, &flags);
	if (wait->deadline_queued) {
		list_del_init(&wait->deadline_node);
		wait->deadline_queued = false;
		task = wait->deadline_task;
		wait->deadline_task = NULL;
	}
	spin_unlock_irqrestore(&queue->lock, flags);
	if (task)
		task_put(task);
}

static void wait_deadline_insert(struct task_wait *wait)
{
	struct wait_deadline_queue *queue;
	struct list_head *pos;
	uint32_t cpu_id;
	irq_flags_t flags;

	if (!wait->deadline.active)
		return;
	/* Record queue locality independently of later Task placement.  CPUs do not
	 * hot-unplug, so the arming CPU remains responsible through removal or
	 * expiry even if the blocked Task is re-homed. */
	cpu_id = current_cpu()->id;
	queue = &deadline_queues[cpu_id];
	spin_lock_irqsave(&queue->lock, &flags);
	BUG_ON(wait->deadline_queued || wait->deadline_task);
	BUG_ON(!task_try_get(wait->owner));
	wait->deadline_cpu = cpu_id;
	wait->deadline_generation = wait->generation;
	wait->deadline_task = wait->owner;
	list_for_each (pos, &queue->entries) {
		struct task_wait *queued =
			list_entry(pos, struct task_wait, deadline_node);

		if (wait->deadline.expires < queued->deadline.expires) {
			__list_add(&wait->deadline_node, pos->prev, pos);
			wait->deadline_queued = true;
			spin_unlock_irqrestore(&queue->lock, flags);
			clockevent_deadline_changed(wait->deadline.expires);
			return;
		}
	}
	list_add_tail(&wait->deadline_node, &queue->entries);
	wait->deadline_queued = true;
	spin_unlock_irqrestore(&queue->lock, flags);
	clockevent_deadline_changed(wait->deadline.expires);
}

/* Mark that an event fired for the given wait generation.  Wakes are hints:
 * wait_block returns WAIT_OUTCOME_EVENT and the caller rechecks its own
 * condition, looping back if no event is actually satisfiable. */
static bool wait_event_fired(struct task_wait *wait, uint64_t generation)
{
	bool wake = false;
	irq_flags_t flags;

	spin_lock_irqsave(&wait->lock, &flags);
	if (wait_active(wait) && wait->generation == generation &&
	    !wait->event_fired) {
		wait->event_fired = true;
		wake = true;
	}
	spin_unlock_irqrestore(&wait->lock, flags);
	return wake;
}

static enum sched_park_result wait_block_current(void)
{
	BUG_ON(!wait_context_can_sleep());
	return sched_block_current(&current_task()->wait);
}

void wait_channel_init(struct wait_channel *channel)
{
	spin_lock_init(&channel->lock, LOCK_RANK_WAIT_CHANNEL,
			LOCK_IRQ_TASK_ONLY);
	INIT_LIST_HEAD(&channel->waiters);
}

static int wait_scope_begin_mode(struct wait_scope *scope, wait_flags_t flags,
				   const struct wait_deadline *deadline,
				   enum task_wait_signal_mode signal_mode,
				   uint64_t signal_set, bool signal_lock_held)
{
	struct task_struct *task = current_task();
	struct task_wait *wait = task ? &task->wait : NULL;
	irq_flags_t irq_flags;

	if (!scope || !deadline || !task)
		return -EINVAL;
	if (!signal_lock_held && !wait_context_can_sleep())
		return -EINVAL;
	if (flags & ~WAIT_FLAG_MASK ||
	    (flags & WAIT_FLAG_MASK) == WAIT_FLAG_MASK)
		return -EINVAL;

	spin_lock_irqsave(&wait->lock, &irq_flags);
	if (scope->active || wait->phase != WAIT_IDLE ||
	    !list_empty(&wait->registrations) ||
	    wait->registration_count != 0) {
		spin_unlock_irqrestore(&wait->lock, irq_flags);
		return -EBUSY;
	}
	wait->phase = WAIT_ARMED;
	wait->policy = (flags & WAIT_FLAG_KILLABLE)
			? TASK_WAIT_KILLABLE
			: (flags & WAIT_FLAG_INTERRUPTIBLE)
				? TASK_WAIT_INTERRUPTIBLE
				: TASK_WAIT_UNINTERRUPTIBLE;
	wait->signal_mode = signal_mode;
	wait->signal_set = signal_set;
	wait->event_fired = false;
	wait->status_value = 0;
	wait->generation++;
	wait->deadline = *deadline;
	wait->owner = task;
	wait->registration_count = 0;
	scope->wait = wait;
	scope->generation = wait->generation;
	scope->active = true;
	spin_unlock_irqrestore(&wait->lock, irq_flags);

	wait_deadline_insert(wait);
	return 0;
}

int wait_scope_begin(struct wait_scope *scope, wait_flags_t flags,
		     const struct wait_deadline *deadline)
{
	return wait_scope_begin_mode(scope, flags, deadline,
				     TASK_WAIT_SIGNAL_DEFAULT, 0, false);
}

int wait_scope_begin_signal_set(struct wait_scope *scope, wait_flags_t flags,
				const struct wait_deadline *deadline,
				uint64_t signal_set)
{
	return wait_scope_begin_mode(scope, flags, deadline, TASK_WAIT_SIGNAL_SET,
				     signal_set, false);
}

int wait_scope_begin_signal_set_locked(struct wait_scope *scope,
					wait_flags_t flags,
					const struct wait_deadline *deadline,
					uint64_t signal_set)
{
	BUG_ON(!spinlock_held());
	return wait_scope_begin_mode(scope, flags, deadline, TASK_WAIT_SIGNAL_SET,
				     signal_set, true);
}

int wait_scope_prepare(struct wait_scope *scope, struct wait_channel *channel,
		       bool exclusive)
{
	struct wait_entry *entry = NULL;
	struct task_struct *task = current_task();
	struct task_wait *wait = scope ? scope->wait : NULL;
	irq_flags_t channel_flags;
	irq_flags_t wait_flags;

	if (!scope || !scope->active || !wait || !channel || !task ||
	    wait != &task->wait)
		return -EINVAL;
	spin_lock_irqsave(&channel->lock, &channel_flags);
	spin_lock_irqsave(&wait->lock, &wait_flags);
	if (wait->phase != WAIT_ARMED || wait->owner != task ||
	    wait->generation != scope->generation) {
		spin_unlock_irqrestore(&wait->lock, wait_flags);
		spin_unlock_irqrestore(&channel->lock, channel_flags);
		return -ECANCELED;
	}
	for (uint32_t i = 0; i < WAIT_MAX_REGISTRATIONS; i++) {
		struct wait_entry *candidate = &wait->entries[i];

		if (candidate->watched == channel) {
			spin_unlock_irqrestore(&wait->lock, wait_flags);
			spin_unlock_irqrestore(&channel->lock, channel_flags);
			return 0;
		}
		if (!candidate->watched && !entry)
			entry = candidate;
	}
	if (!entry) {
		spin_unlock_irqrestore(&wait->lock, wait_flags);
		spin_unlock_irqrestore(&channel->lock, channel_flags);
		return -E2BIG;
	}
	entry->task = task;
	entry->wait = wait;
	entry->watched = channel;
	entry->channel = channel;
	entry->exclusive = exclusive;
	list_add_tail(&entry->channel_node, &channel->waiters);
	list_add_tail(&entry->registration_node, &wait->registrations);
	wait->registration_count++;
	spin_unlock_irqrestore(&wait->lock, wait_flags);
	spin_unlock_irqrestore(&channel->lock, channel_flags);
	return 0;
}

int wait_scope_prepare_current(struct wait_channel *channel, bool exclusive)
{
	struct task_struct *task = current_task();
	struct wait_scope scope;

	if (!task || !channel)
		return -EINVAL;
	scope.wait = &task->wait;
	scope.generation = task->wait.generation;
	scope.active = true;
	return wait_scope_prepare(&scope, channel, exclusive);
}

int wait_scope_block(struct wait_scope *scope, wait_outcome_t *outcome)
{
	struct task_struct *task = current_task();
	struct task_wait *wait = scope ? scope->wait : NULL;

	if (!scope || !scope->active || !wait || !outcome || !task ||
	    wait != &task->wait)
		return -EINVAL;
	if (!wait_context_can_sleep())
		return -EINVAL;
	*outcome = 0;

	for (;;) {
		bool event_fired;
		irq_flags_t flags;

		spin_lock_irqsave(&wait->lock, &flags);
		if (!wait_active(wait) ||
		    wait->generation != scope->generation) {
			spin_unlock_irqrestore(&wait->lock, flags);
			return -EINVAL;
		}
		if (wait->phase == WAIT_ARMED)
			wait->phase = WAIT_BLOCKED;
		event_fired = wait->event_fired;
		if (event_fired)
			wait->event_fired = false;
		spin_unlock_irqrestore(&wait->lock, flags);

		/* Recheck order: fatal -> event -> signal predicate (per policy
		 * class) -> deadline.  Every check is re-derived, never read from
		 * a stored wake reason; wakes are only hints that poll these. */
		if ((wait->policy == TASK_WAIT_INTERRUPTIBLE ||
		     wait->policy == TASK_WAIT_KILLABLE) &&
		    sig_fatal_pending(task))
			return (*outcome = WAIT_OUTCOME_SIGNAL), 0;

		if (event_fired)
			return (*outcome = WAIT_OUTCOME_EVENT), 0;

		if (wait->policy == TASK_WAIT_INTERRUPTIBLE &&
		    sig_wait_ready(task, wait))
			return (*outcome = WAIT_OUTCOME_SIGNAL), 0;

		if (wait->deadline.active && timer_now() >= wait->deadline.expires)
			return (*outcome = WAIT_OUTCOME_TIMEOUT), 0;

		enum sched_park_result park = wait_block_current();

		if (park == SCHED_PARK_INVALID)
			return -EINVAL;
		BUG_ON(park != SCHED_PARKED && park != SCHED_PARK_RACE &&
		       park != SCHED_PARK_STATE_DENIED);
		wait_denial_observe(task, park);
		/* A Task-state denial already crossed a scheduling point.  Return a
		 * recheck hint to the condition owner instead of blindly retrying the
		 * same park: mutexes, futexes, and other wait sources must re-derive
		 * their authoritative condition after every scheduler outcome. */
		if (park == SCHED_PARK_STATE_DENIED)
			return (*outcome = WAIT_OUTCOME_EVENT), 0;
	}
}

static void wait_complete_generation(struct task_wait *wait,
				     uint64_t generation)
{
	irq_flags_t flags;

	if (!wait)
		return;

	spin_lock_irqsave(&wait->lock, &flags);
	BUG_ON(wait->phase != WAIT_ARMED && wait->phase != WAIT_BLOCKED);
	BUG_ON(wait->generation != generation);
	wait->phase = WAIT_COMPLETING;
	spin_unlock_irqrestore(&wait->lock, flags);

	wait_deadline_remove(wait);
	for (;;) {
		struct wait_entry *entry;
		struct wait_channel *channel;
		/* GCC cannot prove the paired `if (channel)` branches agree, so
		 * it flags channel_flags as possibly uninitialized; the value is
		 * only consumed when the lock was actually taken. */
		irq_flags_t channel_flags = 0;
		irq_flags_t wait_flags;

		spin_lock_irqsave(&wait->lock, &wait_flags);
		if (list_empty(&wait->registrations)) {
			struct task_struct *deadline_task = wait->deadline_task;

			wait->registration_count = 0;
			wait->owner = NULL;
			wait->deadline_task = NULL;
				wait->phase = WAIT_IDLE;
				wait->event_fired = false;
				wait->signal_mode = TASK_WAIT_SIGNAL_DEFAULT;
				wait->signal_set = 0;
				wait->deadline = wait_deadline_none();
				IFDEF(CONFIG_DEBUG_CONTEXT,
				      wait_denial_reset_locked(wait);)
			spin_unlock_irqrestore(&wait->lock, wait_flags);
			if (deadline_task)
				task_put(deadline_task);
			return;
		}
		entry = list_first_entry(&wait->registrations,
					struct wait_entry, registration_node);
		channel = entry->channel;
		spin_unlock_irqrestore(&wait->lock, wait_flags);
		if (channel)
			spin_lock_irqsave(&channel->lock, &channel_flags);
		spin_lock_irqsave(&wait->lock, &wait_flags);
		if (entry->wait == wait && entry->channel == channel) {
			list_del_init(&entry->channel_node);
			entry->channel = NULL;
		}
		if (entry->wait == wait) {
			if (!list_empty(&entry->registration_node))
				list_del_init(&entry->registration_node);
			entry->watched = NULL;
			entry->task = NULL;
			entry->wait = NULL;
			entry->channel = NULL;
			if (wait->registration_count != 0)
				wait->registration_count--;
		}
		spin_unlock_irqrestore(&wait->lock, wait_flags);
		if (channel)
			spin_unlock_irqrestore(&channel->lock, channel_flags);
	}
}

void wait_scope_complete(struct wait_scope *scope)
{
	BUG_ON(!scope || !scope->active || !scope->wait);
	wait_complete_generation(scope->wait, scope->generation);
	scope->wait = NULL;
	scope->generation = 0;
	scope->active = false;
}

void wait_scope_cleanup(struct wait_scope *scope)
{
	if (scope && scope->active)
		wait_scope_complete(scope);
}

void wait_cancel_current(void)
{
	struct task_struct *task = current_task();
	struct wait_scope scope;

	if (!task || task->wait.phase == WAIT_IDLE)
		return;
	scope.wait = &task->wait;
	scope.generation = task->wait.generation;
	scope.active = true;
	wait_scope_complete(&scope);
}

static bool wait_channel_wake_match(struct wait_channel *channel,
					    bool exclusive)
{
	struct task_struct *task = NULL;
	struct task_wait *wait;
	struct wait_entry *entry;
	struct list_head *pos;
	uint64_t generation = 0;
	irq_flags_t channel_flags;
	irq_flags_t wait_flags;

	if (!channel)
		return false;
	spin_lock_irqsave(&channel->lock, &channel_flags);
	list_for_each (pos, &channel->waiters) {
		entry = list_entry(pos, struct wait_entry, channel_node);
		if (entry->exclusive != exclusive)
			continue;
		wait = entry->wait;
		if (!wait || !entry->task || entry->channel != channel)
			continue;
		spin_lock_irqsave(&wait->lock, &wait_flags);
		if (!wait_active(wait) || wait->event_fired ||
		    entry->wait != wait || entry->channel != channel) {
			spin_unlock_irqrestore(&wait->lock, wait_flags);
			continue;
		}
		/* A failed try_get means the target is past the point of being
		 * reaped; skip it without taking a reference.  Release the wait
		 * lock first and keep task NULL so the post-loop guard cannot
		 * fall through to a wake on an un-referenced task. */
		if (!task_try_get(entry->task)) {
			spin_unlock_irqrestore(&wait->lock, wait_flags);
			continue;
		}
		task = entry->task;
		generation = wait->generation;
		wait->event_fired = true;
		list_del_init(&entry->channel_node);
		entry->channel = NULL;
		spin_unlock_irqrestore(&wait->lock, wait_flags);
		break;
	}
	spin_unlock_irqrestore(&channel->lock, channel_flags);
	if (!task)
		return false;
	(void)sched_wake(task, generation);
	task_put(task);
	return true;
}

bool wait_channel_wake_one(struct wait_channel *channel)
{
	return wait_channel_wake_match(channel, true);
}

void wait_channel_wake_all(struct wait_channel *channel)
{
	while (wait_channel_wake_match(channel, true))
		;

	while (wait_channel_wake_match(channel, false))
		;
}

bool wait_wake_signal(struct task_struct *task, bool fatal)
{
	struct task_wait *wait;
	uint64_t generation;
	bool eligible;
	irq_flags_t flags;

	if (!task)
		return false;
	wait = &task->wait;
	spin_lock_irqsave(&wait->lock, &flags);
	eligible = wait_active(wait) &&
		((wait->policy == TASK_WAIT_INTERRUPTIBLE) || fatal);
	if (eligible)
		generation = wait->generation;
	else
		generation = 0;
	spin_unlock_irqrestore(&wait->lock, flags);
	if (!eligible || !generation)
		return false;
	(void)sched_wake(task, generation);
	return true;
}

bool wait_wake_event(struct task_struct *task, uint64_t generation)
{
	if (!task || !wait_event_fired(&task->wait, generation))
		return false;
	(void)sched_wake(task, generation);
	return true;
}

bool wait_wake_exit(struct task_struct *task)
{
	struct task_wait *wait;
	uint64_t generation;
	irq_flags_t flags;

	if (!task)
		return false;
	wait = &task->wait;
	spin_lock_irqsave(&wait->lock, &flags);
	if (!wait_active(wait) || wait->event_fired) {
		spin_unlock_irqrestore(&wait->lock, flags);
		return false;
	}
	generation = wait->generation;
	wait->event_fired = true;
	spin_unlock_irqrestore(&wait->lock, flags);
	(void)sched_wake(task, generation);
	return true;
}

void wait_expire_deadlines(uint64_t now)
{
	struct wait_deadline_queue *queue = &deadline_queues[current_cpu()->id];
	irq_flags_t flags;

	for (;;) {
		struct task_wait *wait;
		struct task_struct *task;
		uint64_t generation;

		spin_lock_irqsave(&queue->lock, &flags);
		if (list_empty(&queue->entries)) {
			spin_unlock_irqrestore(&queue->lock, flags);
			break;
		}
		wait = list_first_entry(&queue->entries, struct task_wait,
					deadline_node);
		if (wait->deadline.expires > now) {
			spin_unlock_irqrestore(&queue->lock, flags);
			break;
		}
		list_del_init(&wait->deadline_node);
		wait->deadline_queued = false;
		task = wait->deadline_task;
		generation = wait->deadline_generation;
		spin_unlock_irqrestore(&queue->lock, flags);
		if (task)
			(void)sched_wake(task, generation);
	}
}

uint64_t wait_next_deadline(uint64_t fallback)
{
	struct wait_deadline_queue *queue = &deadline_queues[current_cpu()->id];
	uint64_t next = fallback;
	irq_flags_t flags;

	spin_lock_irqsave(&queue->lock, &flags);
	if (!list_empty(&queue->entries)) {
		struct task_wait *wait = list_first_entry(&queue->entries,
						struct task_wait, deadline_node);
		if (wait->deadline.expires < next)
			next = wait->deadline.expires;
	}
	spin_unlock_irqrestore(&queue->lock, flags);
	return next;
}

int wait_sleep_until(const struct wait_deadline *deadline)
{
	wait_outcome_t outcome;
	int ret;

	if (!deadline || !deadline->active)
		return -EINVAL;
	struct wait_scope scope __wait_scope = {};

	ret = wait_scope_begin(&scope, 0, deadline);
	if (ret < 0)
		return ret;
	ret = wait_scope_block(&scope, &outcome);
	wait_scope_complete(&scope);
	if (ret < 0)
		return ret;
	return outcome == WAIT_OUTCOME_TIMEOUT ? 0 : -EINTR;
}
