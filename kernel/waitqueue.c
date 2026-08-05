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

struct wait_session;

struct wait_timer {
	struct ktimer timer;
	struct wait_session *session;
};

struct wait_entry {
	struct list_head node;
	struct task_struct *task;
	struct wait_channel *channel;
	bool task_ref;
	struct wait_session *session;
	bool session_ref;
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
	refcount_t refs;
	spinlock_t lock;
	enum wait_session_state state;
	struct task_struct *task;
	struct wait_watch_set watch_set;
	struct wait_timer timer;
	bool timer_active;
	bool task_ref;
	bool timer_ref;
	bool active_wait_ref;
	uint32_t registration_refs;
	uint32_t active_operations;
	wait_cancel_fn cancel;
	void *cancel_arg;
};

static void wait_session_get(struct wait_session *session)
{
	BUG_ON(!session);
	refcount_inc(&session->refs);
}

static void wait_session_put(struct wait_session *session)
{
	if (!session)
		return;

	if (!refcount_dec_and_test(&session->refs))
		return;

	BUG_ON(session->state != WAIT_DONE);
	BUG_ON(session->active_wait_ref);
	BUG_ON(session->timer_ref);
	BUG_ON(session->task_ref);
	BUG_ON(session->registration_refs != 0);
	BUG_ON(session->active_operations != 0);
	if (session->watch_set.entries != &session->watch_set.inline_entry)
		kfree(session->watch_set.entries);
	kfree(session);
}

static enum wait_session_state wait_state_get(struct wait_session *session)
{
	enum wait_session_state state;
	irq_flags_t flags;

	spin_lock_irqsave(&session->lock, &flags);
	state = session->state;
	spin_unlock_irqrestore(&session->lock, flags);
	return state;
}

static bool wait_state_is_active_locked(const struct wait_session *session)
{
	return session->state == WAIT_ACTIVE;
}

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
	struct wait_session *session = wait_timer->session;
	struct task_struct *task = session->task;

	(void)arg;
	if (wait_state_get(session) != WAIT_DONE &&
	    wait_task_is_sleeping(task))
		sched_wake_task(task);
}

static void wait_timer_init(struct wait_timer *timer,
			    struct wait_session *session)
{
	BUG_ON(!timer);

	ktimer_init(&timer->timer, wait_timer_fire, NULL);
	timer->session = session;
}

static int wait_timer_start(struct wait_session *session, uint64_t expires)
{
	int ret;
	irq_flags_t flags;

	spin_lock_irqsave(&session->lock, &flags);
	if (!wait_state_is_active_locked(session)) {
		spin_unlock_irqrestore(&session->lock, flags);
		return -ECANCELED;
	}
	if (session->timer_active) {
		spin_unlock_irqrestore(&session->lock, flags);
		return 0;
	}
	session->timer_active = true;
	ret = ktimer_arm(&session->timer.timer, expires, 0);
	if (ret < 0)
		session->timer_active = false;
	spin_unlock_irqrestore(&session->lock, flags);
	return ret;
}

static void wait_timer_cancel(struct wait_session *session)
{
	bool active;
	irq_flags_t flags;
	int ret;

	spin_lock_irqsave(&session->lock, &flags);
	active = session->timer_active;
	session->timer_active = false;
	spin_unlock_irqrestore(&session->lock, flags);
	if (!active)
		return;

	ret = ktimer_cancel_sync(&session->timer.timer);
	BUG_ON(ret < 0);
}

static void init_wait_entry(struct wait_entry *entry, struct task_struct *task);
static int prepare_wait_entry(struct wait_channel *channel,
			      struct wait_entry *entry);
static void finish_wait_entry(struct wait_entry *entry);

static void wait_watch_init(struct wait_watch *registration,
				    struct task_struct *task,
				    struct wait_session *session)
{
	init_wait_entry(&registration->entry, task);
	registration->entry.session = session;
	registration->channel = NULL;
}

static struct wait_session *wait_session_alloc(
					struct task_struct *task,
					const struct wait_request *request,
					uint32_t capacity)
{
	struct wait_session *session;
	uint32_t index;

	session = kzalloc(sizeof(*session), ALLOC_NOWAIT);
	if (!session)
		return NULL;

	refcount_set(&session->refs, 1);
	spin_lock_init(&session->lock);
	session->state = WAIT_ACTIVE;
	session->task = task;
	session->cancel = request ? request->cancel : NULL;
	session->cancel_arg = request ? request->arg : NULL;
	session->watch_set.task = task;
	session->watch_set.capacity = capacity;
	session->watch_set.entries = &session->watch_set.inline_entry;
	if (capacity > 1) {
		session->watch_set.entries =
			kmalloc(sizeof(*session->watch_set.entries) * capacity,
				ALLOC_NOWAIT);
		if (!session->watch_set.entries) {
			kfree(session);
			return NULL;
		}
	}

	for (index = 0; index < capacity; index++)
		wait_watch_init(&session->watch_set.entries[index], task, session);

	if (!task_try_get(task)) {
		if (session->watch_set.entries != &session->watch_set.inline_entry)
			kfree(session->watch_set.entries);
		kfree(session);
		return NULL;
	}
	session->task_ref = true;
	wait_session_get(session);
	session->timer_ref = true;
	wait_timer_init(&session->timer, session);
	return session;
}

static void wait_session_discard(struct wait_session *session)
{
	BUG_ON(!session);
	BUG_ON(session->timer_active);
	session->state = WAIT_DONE;
	if (session->timer_ref) {
		session->timer_ref = false;
		wait_session_put(session);
	}
	if (session->task_ref) {
		task_put(session->task);
		session->task_ref = false;
	}
	wait_session_put(session);
}

static void wait_session_cleanup(struct wait_session *session)
{
	uint32_t index;

	for (index = 0; index < session->watch_set.capacity; index++)
		finish_wait_entry(&session->watch_set.entries[index].entry);
}

static int wait_session_attach(struct wait_session *session)
{
	struct task_struct *task = session->task;
	irq_flags_t flags;

	BUG_ON(!task);
	spin_lock_irqsave(&task->wait_lock, &flags);
	if (task->active_wait) {
		spin_unlock_irqrestore(&task->wait_lock, flags);
		return -EBUSY;
	}
	wait_session_get(session);
	session->active_wait_ref = true;
	task->active_wait = session;
	spin_unlock_irqrestore(&task->wait_lock, flags);
	return 0;
}

static void wait_session_detach(struct wait_session *session)
{
	struct task_struct *task = session->task;
	irq_flags_t flags;
	bool detached = false;

	spin_lock_irqsave(&task->wait_lock, &flags);
	if (task->active_wait == session) {
		task->active_wait = NULL;
		detached = true;
	}
	spin_unlock_irqrestore(&task->wait_lock, flags);
	if (detached) {
		session->active_wait_ref = false;
		wait_session_put(session);
	}
}

void wait_cancel_task(struct task_struct *task)
{
	struct wait_session *session;
	irq_flags_t flags;

	if (!task)
		return;
	if (!task_try_get(task))
		return;

	spin_lock_irqsave(&task->wait_lock, &flags);
	session = task->active_wait;
	if (session)
		wait_session_get(session);
	spin_unlock_irqrestore(&task->wait_lock, flags);
	if (!session) {
		task_put(task);
		return;
	}

	BUG_ON(!sched_context_can_schedule());
	BUG_ON(task == current_task());
	spin_lock_irqsave(&session->lock, &flags);
	if (session->state == WAIT_ACTIVE)
		session->state = WAIT_CANCEL;
	spin_unlock_irqrestore(&session->lock, flags);
	if (wait_task_is_sleeping(task))
		sched_wake_task(task);

	while (wait_state_get(session) != WAIT_DONE)
		schedule();

	wait_session_put(session);
	task_put(task);
}

int wait_session_watch(struct wait_session *session,
		       struct wait_channel *channel)
{
	struct wait_watch_set *watch_set;
	struct wait_watch *free_registration = NULL;
	struct wait_watch *registration;
	irq_flags_t flags;
	uint32_t index;
	int ret;

	if (!session || !channel)
		return -EINVAL;

	spin_lock_irqsave(&session->lock, &flags);
	if (!wait_state_is_active_locked(session)) {
		spin_unlock_irqrestore(&session->lock, flags);
		return -ECANCELED;
	}
	watch_set = &session->watch_set;

	for (index = 0; index < watch_set->capacity; index++) {
		registration = &watch_set->entries[index];
		if (registration->channel == channel) {
			if (!registration->entry.channel) {
				ret = prepare_wait_entry(channel, &registration->entry);
				spin_unlock_irqrestore(&session->lock, flags);
				return ret;
			}
			spin_unlock_irqrestore(&session->lock, flags);
			return 0;
		}
		if (!registration->channel && !free_registration)
			free_registration = registration;
	}

	if (!free_registration) {
		spin_unlock_irqrestore(&session->lock, flags);
		return -E2BIG;
	}
	wait_session_get(session);
	free_registration->entry.session_ref = true;
	session->registration_refs++;
	ret = prepare_wait_entry(channel, &free_registration->entry);
	if (ret < 0) {
		free_registration->entry.session_ref = false;
		BUG_ON(session->registration_refs == 0);
		session->registration_refs--;
		wait_session_put(session);
		spin_unlock_irqrestore(&session->lock, flags);
		return ret;
	}
	free_registration->channel = channel;
	spin_unlock_irqrestore(&session->lock, flags);
	return 0;
}

static bool wait_session_operation_begin(struct wait_session *session)
{
	irq_flags_t flags;
	bool active;

	spin_lock_irqsave(&session->lock, &flags);
	active = wait_state_is_active_locked(session);
	if (active) {
		session->active_operations++;
		wait_session_get(session);
	}
	spin_unlock_irqrestore(&session->lock, flags);
	return active;
}

static void wait_session_operation_end(struct wait_session *session)
{
	irq_flags_t flags;

	spin_lock_irqsave(&session->lock, &flags);
	BUG_ON(session->active_operations == 0);
	session->active_operations--;
	spin_unlock_irqrestore(&session->lock, flags);
	wait_session_put(session);
}

static int wait_check(const struct wait_request *request,
		      struct wait_session *session)
{
	int ret;

	if (!request)
		return 0;
	if (!wait_session_operation_begin(session))
		return -ECANCELED;
	ret = request->check(session, request->arg);
	wait_session_operation_end(session);
	return ret;
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

static int wait_session_complete(struct wait_session *session, int ret)
{
	bool cancelled;
	irq_flags_t flags;

	spin_lock_irqsave(&session->lock, &flags);
	if (session->state == WAIT_CANCEL) {
		cancelled = true;
		ret = -ECANCELED;
	} else {
		BUG_ON(session->state != WAIT_ACTIVE);
		cancelled = false;
	}
	BUG_ON(session->active_operations != 0);
	session->state = WAIT_COMPLETING;
	spin_unlock_irqrestore(&session->lock, flags);

	wait_finish_current_state();
	wait_timer_cancel(session);
	wait_session_cleanup(session);
	if (cancelled && session->cancel)
		session->cancel(session->cancel_arg);

	if (session->task_ref) {
		task_put(session->task);
		session->task_ref = false;
	}
	if (session->timer_ref) {
		session->timer_ref = false;
		wait_session_put(session);
	}

	spin_lock_irqsave(&session->lock, &flags);
	BUG_ON(session->state != WAIT_COMPLETING);
	session->state = WAIT_DONE;
	spin_unlock_irqrestore(&session->lock, flags);
	wait_session_detach(session);
	return ret;
}

int wait_for(const struct wait_request *request, wait_flags_t flags,
		     const struct wait_deadline *deadline, wait_outcome_t *outcome)
{
	struct wait_session *session;
	wait_outcome_t result;
	uint32_t sleep_state;
	uint32_t capacity;
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
	if ((flags & WAIT_FLAG_MASK) == WAIT_FLAG_MASK)
		return -EINVAL;
	if (request && (!request->check || request->channel_limit == 0 ||
			request->channel_limit > WAIT_SESSION_MAX_CHANNELS))
		return -EINVAL;
	if (!request && !deadline->active && !(flags & WAIT_FLAG_INTERRUPTIBLE))
		return -EINVAL;

	capacity = request ? request->channel_limit : 1;
	session = wait_session_alloc(current_task(), request, capacity);
	if (!session)
		return -ENOMEM;
	ret = wait_session_attach(session);
	if (ret < 0) {
		wait_session_discard(session);
		return ret;
	}

	for (;;) {
		probe_result = wait_check(request, session);
		if (probe_result < 0) {
			ret = probe_result;
			break;
		}
		if (wait_state_get(session) == WAIT_CANCEL) {
			ret = -ECANCELED;
			break;
		}
		result = wait_arbitrate(probe_result, flags, deadline);
		if (result) {
			*outcome = result;
			ret = 0;
			break;
		}

		if (deadline->active) {
			ret = wait_timer_start(session, deadline->expires);
			if (ret < 0)
				break;
		}

		if (flags & WAIT_FLAG_KILLABLE)
			sleep_state = TASK_KILLABLE;
		else if (flags & WAIT_FLAG_INTERRUPTIBLE)
			sleep_state = TASK_INTERRUPTIBLE;
		else
			sleep_state = TASK_UNINTERRUPTIBLE;
		task_set_state(current_task(), sleep_state);

		probe_result = wait_check(request, session);
		if (probe_result < 0) {
			ret = probe_result;
			break;
		}
		if (wait_state_get(session) == WAIT_CANCEL) {
			ret = -ECANCELED;
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

	ret = wait_session_complete(session, ret);
	if (ret < 0)
		*outcome = 0;
	wait_session_put(session);
	return ret;
}

int wait_event_uninterruptible(const struct wait_request *request)
{
	const struct wait_deadline deadline = wait_deadline_none();
	wait_outcome_t outcome;
	int ret;

	ret = wait_for_uninterruptible(request, &deadline, &outcome);
	if (ret < 0)
		return ret;
	BUG_ON(outcome != WAIT_OUTCOME_EVENT);
	return 0;
}

int wait_sleep_until(const struct wait_deadline *deadline)
{
	wait_outcome_t outcome;
	int ret;

	ret = wait_for_uninterruptible(NULL, deadline, &outcome);
	if (ret < 0)
		return ret;
	BUG_ON(outcome != WAIT_OUTCOME_TIMEOUT);
	return 0;
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
	entry->session = NULL;
	entry->session_ref = false;
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
	struct wait_session *session;
	bool put_task;
	bool put_session;
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
	session = entry->session;
	put_session = entry->session_ref;
	entry->channel = NULL;
	entry->task_ref = false;
	entry->session_ref = false;
	if (put_task)
		task_put(task);
	if (put_session) {
		spin_lock_irqsave(&session->lock, &flags);
		BUG_ON(session->registration_refs == 0);
		session->registration_refs--;
		spin_unlock_irqrestore(&session->lock, flags);
		wait_session_put(session);
	}
}

bool wait_channel_wake_one(struct wait_channel *channel)
{
	struct wait_entry *entry;
	struct task_struct *task = NULL;
	struct wait_session *session = NULL;
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
		session = entry->session;
		BUG_ON(!session || !entry->session_ref);
		wait_session_get(session);
		list_del_init(&entry->node);
		entry->channel = NULL;
		break;
	}
	spin_unlock_irqrestore(&channel->lock, flags);

	if (task) {
		sched_wake_task(task);
		task_put(task);
		wait_session_put(session);
	}
	return task != NULL;
}

void wait_channel_wake_all(struct wait_channel *channel)
{
	while (wait_channel_wake_one(channel))
		;
}
