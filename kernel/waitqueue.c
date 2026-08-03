/*
 * kernel/waitqueue.c - kernel wait queues
 */

#include <kernel/errno.h>
#include <kernel/sched.h>
#include <kernel/signal.h>
#include <kernel/slab.h>
#include <kernel/task.h>
#include <kernel/time.h>
#include <kernel/timer.h>
#include <kernel/tools.h>
#include <kernel/wait.h>
#include <kernel/irq.h>
#include <kernel/processor.h>

struct wait_timer {
	struct ktimer timer;
	struct task_struct *task;
};

struct wait_entry {
	struct list_head node;
	struct task_struct *task;
	struct wait_channel *channel;
	bool task_ref;
};

struct wait_watch {
	struct wait_entry entry;
	struct wait_channel *channel;
};

struct wait_watch_set {
	struct task_struct *task;
	struct wait_watch *entries;
	uint32_t capacity;
	struct wait_watch inline_entry;
};

struct wait_session {
	struct wait_watch_set *watch_set;
	struct wait_timer *timer;
	bool *timer_active;
	wait_cancel_fn cancel;
	void *cancel_arg;
};

static void wait_finish_current_state(void)
{
	if (!current_task())
		return;

	if (task_state(current_task()) & TASK_ANY_SLEEP)
		task_mark_running(current_task());
}

static bool wait_task_is_sleeping(struct task_struct *task)
{
	return (task_state(task) & TASK_ANY_SLEEP) != 0;
}

static void wait_timer_fire(struct ktimer *timer, void *arg)
{
	struct wait_timer *wait_timer =
		container_of(timer, struct wait_timer, timer);

	(void)arg;
	if (wait_task_is_sleeping(wait_timer->task))
		sched_wake_task(wait_timer->task);
}

static void wait_timer_init(struct wait_timer *timer, struct task_struct *task)
{
	BUG_ON(!timer);

	ktimer_init(&timer->timer, wait_timer_fire, NULL);
	timer->task = task;
}

static int wait_timer_start(struct wait_timer *timer, uint64_t expires)
{
	BUG_ON(!timer);
	BUG_ON(!timer->task);

	return ktimer_arm(&timer->timer, expires, 0);
}

static void wait_timer_cancel(struct wait_timer *timer)
{
	bool cancelled;

	if (!timer)
		return;

	cancelled = ktimer_cancel(&timer->timer);
	(void)cancelled;
}

static void init_wait_entry(struct wait_entry *entry, struct task_struct *task);
static int prepare_wait_entry(struct wait_channel *channel,
			      struct wait_entry *entry);
static void finish_wait_entry(struct wait_entry *entry);

static void wait_watch_init(struct wait_watch *registration,
			    struct task_struct *task)
{
	init_wait_entry(&registration->entry, task);
	registration->channel = NULL;
}

static int wait_session_init(struct wait_watch_set *session,
			     struct task_struct *task, uint32_t capacity)
{
	uint32_t index;

	session->task = task;
	session->capacity = capacity;
	session->entries = &session->inline_entry;
	if (capacity > 1) {
		session->entries =
			kmalloc(sizeof(*session->entries) * capacity);
		if (!session->entries)
			return -ENOMEM;
	}

	for (index = 0; index < capacity; index++)
		wait_watch_init(&session->entries[index], task);
	return 0;
}

static void wait_session_cleanup(struct wait_watch_set *session)
{
	uint32_t index;

	for (index = 0; index < session->capacity; index++)
		finish_wait_entry(&session->entries[index].entry);
	if (session->entries != &session->inline_entry)
		kfree(session->entries);
	session->entries = NULL;
}

static void wait_session_attach(struct wait_session *session,
				const struct wait_request *request,
				struct wait_watch_set *watch_set,
				struct wait_timer *timer, bool *timer_active)
{
	struct task_struct *task = current_task();

	BUG_ON(!task);
	BUG_ON(task->active_wait);
	session->watch_set = watch_set;
	session->timer = timer;
	session->timer_active = timer_active;
	session->cancel = request ? request->cancel : NULL;
	session->cancel_arg = request ? request->arg : NULL;
	task->active_wait = session;
}

static void wait_session_detach(struct wait_session *session)
{
	struct task_struct *task = current_task();

	if (task && task->active_wait == session)
		task->active_wait = NULL;
}

void wait_cancel_task(struct task_struct *task)
{
	struct wait_session *session;

	if (!task)
		return;
	session = task->active_wait;
	if (!session)
		return;

	task->active_wait = NULL;
	if (*session->timer_active) {
		wait_timer_cancel(session->timer);
		*session->timer_active = false;
	}
	wait_session_cleanup(session->watch_set);
	if (session->cancel)
		session->cancel(session->cancel_arg);
}

int wait_session_watch(struct wait_session *session,
		       struct wait_channel *channel)
{
	struct wait_watch_set *watch_set;
	struct wait_watch *free_registration = NULL;
	struct wait_watch *registration;
	uint32_t index;

	if (!session || !session->watch_set || !channel)
		return -EINVAL;
	watch_set = session->watch_set;

	for (index = 0; index < watch_set->capacity; index++) {
		registration = &watch_set->entries[index];
		if (registration->channel == channel) {
			if (!registration->entry.channel)
				return prepare_wait_entry(channel,
							  &registration->entry);
			return 0;
		}
		if (!registration->channel && !free_registration)
			free_registration = registration;
	}

	if (!free_registration)
		return -E2BIG;
	{
		int ret = prepare_wait_entry(channel, &free_registration->entry);

		if (ret < 0)
			return ret;
	}
	free_registration->channel = channel;
	return 0;
}

static int wait_check(const struct wait_request *request,
		      struct wait_session *session)
{
	if (!request)
		return 0;
	return request->check(session, request->arg);
}

static wait_outcome_t wait_arbitrate(int probe_result, wait_flags_t flags,
				     const struct wait_deadline *deadline)
{
	if (probe_result > 0)
		return WAIT_OUTCOME_EVENT;
	if ((flags & WAIT_FLAG_INTERRUPTIBLE) && signal_pending(current_task()))
		return WAIT_OUTCOME_SIGNAL;
	if ((flags & WAIT_FLAG_KILLABLE) &&
	    signal_fatal_pending(current_task()))
		return WAIT_OUTCOME_SIGNAL;
	if (deadline->active && timer_now() >= deadline->expires)
		return WAIT_OUTCOME_TIMEOUT;
	return 0;
}

static void wait_block_current(uint32_t sleep_state)
{
	bool enabled_irq_for_sleep = false;

	BUG_ON(!wait_context_can_sleep());
	if (task_state(current_task()) != sleep_state)
		return;
	if (sched_has_runnable()) {
		schedule();
		return;
	}

	if (irqs_disabled()) {
		local_irq_enable();
		enabled_irq_for_sleep = true;
	}
	wait_for_interrupt();
	if (enabled_irq_for_sleep)
		local_irq_disable();
}

int wait_for(const struct wait_request *request, wait_flags_t flags,
	     const struct wait_deadline *deadline, wait_outcome_t *outcome)
{
	struct wait_watch_set watch_set;
	struct wait_timer timer;
	struct wait_session session;
	wait_outcome_t result;
	uint32_t sleep_state;
	uint32_t capacity;
	bool timer_active = false;
	int probe_result;
	int ret;

	BUG_ON(!wait_context_can_sleep());
	if (!outcome)
		return -EINVAL;
	*outcome = 0;
	if (!deadline || !current_task())
		return -EINVAL;
	if (task_state(current_task()) != TASK_RUNNING) {
		wait_finish_current_state();
		return -EINVAL;
	}
	if (flags & ~WAIT_FLAG_MASK)
		return -EINVAL;
	if (request && (!request->check || request->channel_limit == 0 ||
			request->channel_limit > WAIT_SESSION_MAX_CHANNELS))
		return -EINVAL;
	if (!request && !deadline->active && !(flags & WAIT_FLAG_INTERRUPTIBLE))
		return -EINVAL;

	capacity = request ? request->channel_limit : 1;
	ret = wait_session_init(&watch_set, current_task(), capacity);
	if (ret < 0)
		return ret;
	wait_timer_init(&timer, current_task());
	wait_session_attach(&session, request, &watch_set, &timer, &timer_active);

	for (;;) {
		probe_result = wait_check(request, &session);
		if (probe_result < 0) {
			ret = probe_result;
			break;
		}
		result = wait_arbitrate(probe_result, flags, deadline);
		if (result) {
			*outcome = result;
			ret = 0;
			break;
		}

		if (deadline->active && !timer_active) {
			ret = wait_timer_start(&timer, deadline->expires);
			if (ret < 0)
				break;
			timer_active = true;
		}

		if (flags & WAIT_FLAG_KILLABLE)
			sleep_state = TASK_KILLABLE;
		else if (flags & WAIT_FLAG_INTERRUPTIBLE)
			sleep_state = TASK_INTERRUPTIBLE;
		else
			sleep_state = TASK_UNINTERRUPTIBLE;
		task_set_state(current_task(), sleep_state);

		probe_result = wait_check(request, &session);
		if (probe_result < 0) {
			ret = probe_result;
			break;
		}
		result = wait_arbitrate(probe_result, flags, deadline);
		if (result) {
			*outcome = result;
			ret = 0;
			break;
		}

		wait_block_current(sleep_state);
		wait_finish_current_state();
	}

	wait_session_detach(&session);
	wait_finish_current_state();
	if (timer_active)
		wait_timer_cancel(&timer);
	wait_session_cleanup(&watch_set);
	if (ret < 0)
		*outcome = 0;
	return ret;
}

void wait_channel_init(struct wait_channel *channel)
{
	spin_lock_init(&channel->lock);
	INIT_LIST_HEAD(&channel->waiters);
}

static void init_wait_entry(struct wait_entry *entry, struct task_struct *task)
{
	BUG_ON(!entry);

	INIT_LIST_HEAD(&entry->node);
	entry->task = task;
	entry->channel = NULL;
	entry->task_ref = false;
}

static int prepare_wait_entry(struct wait_channel *channel,
			      struct wait_entry *entry)
{
	bool acquired_ref = false;
	bool attached = false;
	irq_flags_t flags;

	if (!channel || !entry)
		return -EINVAL;

	if (!entry->task_ref) {
		if (!task_try_get(entry->task))
			return -ESRCH;
		acquired_ref = true;
	}

	spin_lock_irqsave(&channel->lock, &flags);
	if (!entry->channel && list_empty(&entry->node)) {
		entry->channel = channel;
		entry->task_ref = true;
		list_add_tail(&entry->node, &channel->waiters);
		attached = true;
	}
	spin_unlock_irqrestore(&channel->lock, flags);

	if (acquired_ref && !attached)
		task_put(entry->task);
	return 0;
}

static void finish_wait_entry(struct wait_entry *entry)
{
	struct wait_channel *channel;
	struct task_struct *task;
	bool put_task;
	irq_flags_t flags;

	if (!entry)
		return;

	channel = entry->channel;
	if (channel) {
		spin_lock_irqsave(&channel->lock, &flags);
		if (entry->channel == channel && !list_empty(&entry->node)) {
			list_del_init(&entry->node);
			entry->channel = NULL;
		}
		spin_unlock_irqrestore(&channel->lock, flags);
	}

	put_task = entry->task_ref;
	task = entry->task;
	entry->channel = NULL;
	entry->task_ref = false;
	if (put_task)
		task_put(task);
}

bool wait_channel_wake_one(struct wait_channel *channel)
{
	struct wait_entry *entry;
	struct task_struct *task = NULL;
	struct list_head *pos;
	struct list_head *next;
	irq_flags_t flags;

	spin_lock_irqsave(&channel->lock, &flags);
	list_for_each_safe (pos, next, &channel->waiters) {
		entry = list_entry(pos, struct wait_entry, node);
		if (!wait_task_is_sleeping(entry->task))
			continue;

		BUG_ON(!task_try_get(entry->task));
		task = entry->task;
		list_del_init(&entry->node);
		entry->channel = NULL;
		break;
	}
	spin_unlock_irqrestore(&channel->lock, flags);

	if (task) {
		sched_wake_task(task);
		task_put(task);
	}
	return task != NULL;
}

void wait_channel_wake_all(struct wait_channel *channel)
{
	while (wait_channel_wake_one(channel))
		;
}
