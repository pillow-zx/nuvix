#include <nuvix/errno.h>
#include <nuvix/sched.h>
#include <nuvix/signal.h>
#include <nuvix/task.h>
#include <nuvix/timer.h>
#include <nuvix/wait.h>
#include <nuvix/event.h>

struct wait_deadline_queue {
	spinlock_t lock;
	struct list_head entries;
};
static struct wait_deadline_queue deadline_queues[NR_CPUS];

void wait_init(void)
{
	for (uint32_t id = 0; id < NR_CPUS; id++) {
		spin_lock_init(&deadline_queues[id].lock);
		INIT_LIST_HEAD(&deadline_queues[id].entries);
	}
}

bool wait_may_block(void)
{
	return current_task() && wait_context_can_sleep() &&
		current_task()->wait.phase == WAIT_IDLE;
}

void wait_channel_init(struct wait_channel *channel)
{
	spin_lock_init(&channel->lock);
	INIT_LIST_HEAD(&channel->waiters);
	INIT_LIST_HEAD(&channel->subscriptions);
}

int poll_wait(struct poll_table *table, struct wait_channel *source)
{
	return table ? table->queue(table, source) : 0;
}

void event_subscribe(struct event_subscription *sub, struct wait_channel *source)
{
	irq_flags_t flags;
	BUG_ON(sub->source);
	spin_lock_irqsave(&source->lock, flags);
	sub->source = source;
	list_add_tail(&sub->node, &source->subscriptions);
	spin_unlock_irqrestore(&source->lock, flags);
}

void event_unsubscribe(struct event_subscription *sub)
{
	struct wait_channel *source = sub->source;
	irq_flags_t flags;
	if (!source)
		return;
	spin_lock_irqsave(&source->lock, flags);
	list_del_init(&sub->node);
	sub->source = NULL;
	spin_unlock_irqrestore(&source->lock, flags);
}

static void event_notify(struct wait_channel *source)
{
	struct event_subscription *sub;
	irq_flags_t flags;
	spin_lock_irqsave(&source->lock, flags);
	list_for_each_entry(sub, &source->subscriptions, node)
		sub->notify(sub);
	spin_unlock_irqrestore(&source->lock, flags);
}

static void deadline_insert(struct task_wait *wait)
{
	struct wait_deadline_queue *queue;
	struct list_head *pos;
	irq_flags_t flags;

	if (!wait->deadline.active)
		return;
	flags = local_irq_save();
	wait->deadline_cpu = current_cpu()->id;
	queue = &deadline_queues[wait->deadline_cpu];
	spin_lock(&queue->lock);
	BUG_ON(wait->deadline_queued || wait->deadline_task);
	BUG_ON(!task_try_get(wait->owner));
	wait->deadline_task = wait->owner;
	wait->deadline_generation = wait->generation;
	wait->deadline_queued = true;
	list_for_each(pos, &queue->entries) {
		struct task_wait *other = list_entry(pos, struct task_wait,
						    deadline_node);
		if (other->deadline.expires > wait->deadline.expires)
			break;
	}
	__list_add(&wait->deadline_node, pos->prev, pos);
	spin_unlock(&queue->lock);
	clockevent_deadline_changed(wait->deadline.expires);
	local_irq_restore(flags);
}

static void deadline_remove(struct task_wait *wait)
{
	struct wait_deadline_queue *queue = &deadline_queues[wait->deadline_cpu];
	struct task_struct *task = NULL;
	irq_flags_t flags;

	spin_lock_irqsave(&queue->lock, flags);
	if (wait->deadline_queued) {
		list_del_init(&wait->deadline_node);
		wait->deadline_queued = false;
		task = wait->deadline_task;
		wait->deadline_task = NULL;
	}
	spin_unlock_irqrestore(&queue->lock, flags);
	task_put(task);
}

static int begin(struct wait_scope *scope, wait_flags_t flags,
		 const struct wait_deadline *deadline,
		 enum task_wait_signal_mode mode, uint64_t set, bool locked)
{
	struct task_struct *task = current_task();
	struct task_wait *wait;

	if (!scope || !task || !deadline || (!locked && !wait_context_can_sleep()))
		return -EINVAL;
	if ((flags & ~WAIT_FLAG_MASK) || (flags & WAIT_FLAG_MASK) == WAIT_FLAG_MASK)
		return -EINVAL;
	wait = &task->wait;
	if (scope->active || wait->phase != WAIT_IDLE) {
		return -EBUSY;
	}
	wait->policy = flags & WAIT_FLAG_KILLABLE ? TASK_WAIT_KILLABLE :
		flags & WAIT_FLAG_INTERRUPTIBLE ? TASK_WAIT_INTERRUPTIBLE :
		TASK_WAIT_UNINTERRUPTIBLE;
	wait->phase = WAIT_ARMED;
	wait->signal_mode = mode;
	wait->signal_set = set;
	wait->status_value = 0;
	wait->owner = task;
	wait->deadline = *deadline;
	wait->entries = scope->entries ? scope->entries : &scope->single;
	wait->capacity = scope->entries ? scope->capacity : 1;
	wait->registration_count = 0;
	scope->wait = wait;
	scope->generation = sched_park_arm(task, wait->policy,
		mode == TASK_WAIT_SIGNAL_SET ? set : 0);
	wait->generation = scope->generation;
	scope->active = true;
	deadline_insert(wait);
	return 0;
}

int wait_scope_begin(struct wait_scope *scope, wait_flags_t flags,
		     const struct wait_deadline *deadline)
{
	return begin(scope, flags, deadline, TASK_WAIT_SIGNAL_DEFAULT, 0, false);
}
int wait_scope_begin_signal_set(struct wait_scope *scope, wait_flags_t flags,
				const struct wait_deadline *deadline, uint64_t set)
{
	return begin(scope, flags, deadline, TASK_WAIT_SIGNAL_SET, set, false);
}
int wait_scope_begin_signal_set_locked(struct wait_scope *scope,
				      wait_flags_t flags,
				      const struct wait_deadline *deadline,
				      uint64_t set)
{
	return begin(scope, flags, deadline, TASK_WAIT_SIGNAL_SET, set, true);
}

int wait_scope_prepare(struct wait_scope *scope, struct wait_channel *channel,
		       bool exclusive)
{
	struct task_wait *wait = scope->wait;
	struct wait_entry *entry;
	irq_flags_t flags;

	if (!scope->active || !channel || wait != &current_task()->wait)
		return -EINVAL;
	for (size_t i = 0; i < wait->registration_count; i++)
		if (wait->entries[i].channel == channel)
			return 0;
	if (wait->registration_count == wait->capacity)
		return -E2BIG;
	entry = &wait->entries[wait->registration_count++];
	entry->task = current_task();
	entry->channel = channel;
	entry->generation = scope->generation;
	entry->exclusive = exclusive;
	INIT_LIST_HEAD(&entry->channel_node);
	spin_lock_irqsave(&channel->lock, flags);
	list_add_tail(&entry->channel_node, &channel->waiters);
	spin_unlock_irqrestore(&channel->lock, flags);
	return 0;
}

int wait_scope_prepare_current(struct wait_channel *channel, bool exclusive)
{
	struct task_wait *wait = &current_task()->wait;
	struct wait_scope scope = {
		.wait = wait, .generation = wait->generation,
		.active = wait->phase != WAIT_IDLE,
	};
	return wait_scope_prepare(&scope, channel, exclusive);
}

int wait_scope_block(struct wait_scope *scope, wait_outcome_t *outcome)
{
	struct task_struct *task = current_task();
	struct task_wait *wait = &task->wait;

	if (!scope->active || scope->wait != wait || !wait_context_can_sleep())
		return -EINVAL;
	for (;;) {
		wait->phase = WAIT_BLOCKED;
		if (sched_take_event(task)) {
			*outcome = WAIT_OUTCOME_EVENT;
			return 0;
		}
		if ((wait->policy == TASK_WAIT_INTERRUPTIBLE &&
		     sig_wait_ready(task, wait)) ||
		    (wait->policy == TASK_WAIT_KILLABLE && sig_fatal_pending(task))) {
			*outcome = WAIT_OUTCOME_SIGNAL;
			return 0;
		}
		if (wait->deadline.active && timer_now() >= wait->deadline.expires) {
			*outcome = WAIT_OUTCOME_TIMEOUT;
			return 0;
		}
		(void)sched_block_current(scope->generation);
	}
}

void wait_scope_complete(struct wait_scope *scope)
{
	struct task_struct *task = current_task();
	struct task_wait *wait = scope->wait;
	irq_flags_t flags;

	BUG_ON(!scope->active || wait != &task->wait);
	deadline_remove(wait);
	for (size_t i = 0; i < wait->registration_count; i++) {
		struct wait_entry *entry = &wait->entries[i];
		struct wait_channel *channel = entry->channel;

		spin_lock_irqsave(&channel->lock, flags);
		list_del_init(&entry->channel_node);
		spin_unlock_irqrestore(&channel->lock, flags);
	}
	sched_park_finish(task);
	wait->phase = WAIT_IDLE;
	wait->entries = NULL;
	wait->capacity = 0;
	wait->registration_count = 0;
	wait->deadline = wait_deadline_none();
	scope->active = false;
	scope->wait = NULL;
}

void wait_scope_cleanup(struct wait_scope *scope)
{
	if (scope->active)
		wait_scope_complete(scope);
}

void wait_cancel_current(void)
{
	struct task_wait *wait = &current_task()->wait;
	struct wait_scope scope = {
		.wait = wait, .generation = wait->generation,
		.active = wait->phase != WAIT_IDLE,
	};
	if (scope.active)
		wait_scope_complete(&scope);
}

bool wait_wake_event(struct task_struct *task, uint64_t generation)
{
	return sched_wake_event(task, generation);
}

bool wait_wake_signal(struct task_struct *task, bool fatal)
{
	return sched_wake_signal(task, fatal);
}

bool wait_wake_exit(struct task_struct *task)
{
	/* An exit request is persistent; uninterruptible operations finish first. */
	(void)wait_wake_signal(task, true);
	sched_resume(task);
	return true;
}

static bool channel_wake(struct wait_channel *channel, bool exclusive_only)
{
	struct wait_entry *entry;
	struct task_struct *task = NULL;
	uint64_t generation = 0;
	irq_flags_t flags;

	spin_lock_irqsave(&channel->lock, flags);
	list_for_each_entry(entry, &channel->waiters, channel_node) {
		if (exclusive_only && !entry->exclusive)
			continue;
		task = entry->task;
		generation = entry->generation;
		BUG_ON(!task_try_get(task));
		list_del_init(&entry->channel_node);
		break;
	}
	spin_unlock_irqrestore(&channel->lock, flags);
	if (!task)
		return false;
	(void)wait_wake_event(task, generation);
	task_put(task);
	return true;
}

bool wait_channel_wake_one(struct wait_channel *channel)
{
	event_notify(channel);
	return channel_wake(channel, true);
}
void wait_channel_wake_all(struct wait_channel *channel)
{
	event_notify(channel);
	while (channel_wake(channel, false))
		;
}

void wait_expire_deadlines(uint64_t now)
{
	struct wait_deadline_queue *queue = &deadline_queues[current_cpu()->id];

	for (;;) {
		struct task_wait *wait;
		struct task_struct *task;
		uint64_t generation;
		irq_flags_t flags;

		spin_lock_irqsave(&queue->lock, flags);
		if (list_empty(&queue->entries)) {
			spin_unlock_irqrestore(&queue->lock, flags);
			return;
		}
		wait = list_first_entry(&queue->entries, struct task_wait, deadline_node);
		if (wait->deadline.expires > now) {
			spin_unlock_irqrestore(&queue->lock, flags);
			return;
		}
		list_del_init(&wait->deadline_node);
		wait->deadline_queued = false;
		task = wait->deadline_task;
		generation = wait->deadline_generation;
		wait->deadline_task = NULL;
		spin_unlock_irqrestore(&queue->lock, flags);
		(void)sched_wake(task, generation);
		task_put(task);
	}
}

uint64_t wait_next_deadline(uint64_t fallback)
{
	struct wait_deadline_queue *queue = &deadline_queues[current_cpu()->id];
	irq_flags_t flags;

	spin_lock_irqsave(&queue->lock, flags);
	if (!list_empty(&queue->entries)) {
		struct task_wait *wait = list_first_entry(&queue->entries,
							struct task_wait, deadline_node);
		if (wait->deadline.expires < fallback)
			fallback = wait->deadline.expires;
	}
	spin_unlock_irqrestore(&queue->lock, flags);
	return fallback;
}

int wait_sleep_until(const struct wait_deadline *deadline)
{
	struct wait_scope scope __wait_scope = {};
	wait_outcome_t outcome;
	int ret = wait_scope_begin(&scope, 0, deadline);

	if (ret < 0)
		return ret;
	ret = wait_scope_block(&scope, &outcome);
	wait_scope_complete(&scope);
	return ret < 0 ? ret : outcome == WAIT_OUTCOME_TIMEOUT ? 0 : -EINTR;
}
