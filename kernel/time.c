/*
 * kernel/time.c - 时间子系统核心实现
 */

#include <kernel/errno.h>
#include <kernel/bitops.h>
#include <kernel/list.h>
#include <kernel/signal.h>
#include <kernel/sched.h>
#include <kernel/slab.h>
#include <kernel/spinlock.h>
#include <kernel/task.h>
#include <kernel/time.h>
#include <kernel/timer.h>
#include <kernel/types.h>
#include <kernel/wait.h>
#include <kernel/processor.h>

constexpr uint64_t USEC_PER_SEC = 1000000ULL;
constexpr int64_t NSEC_PER_SEC = 1000000000LL;

struct realtime_clock {
	spinlock_t lock;
	struct timespec offset;
};

static struct realtime_clock realtime_clock = {
	.lock = SPINLOCK_INIT,
};

static struct {
	spinlock_t lock;
	struct list_head entries;
} ktimer_queue = {
	.lock = SPINLOCK_INIT,
	.entries = LIST_HEAD_INIT(ktimer_queue.entries),
};

struct ktimer_cancel_waiter {
	struct list_head node;
	struct task_struct *task;
	bool queued;
	bool ref;
};

static void ktimer_wake_waiters(struct list_head *waiters)
{
	for (;;) {
		struct ktimer_cancel_waiter *waiter;
		struct task_struct *task;

		if (list_empty(waiters))
			return;
		waiter = list_first_entry(waiters,
					  struct ktimer_cancel_waiter, node);
		BUG_ON(!task_try_get(waiter->task));
		list_del_init(&waiter->node);
		waiter->queued = false;
		task = waiter->task;

		sched_wake_task(task);
		task_put(task);
	}
}

static uint64_t nsec_from_mtime_remainder(uint64_t ticks)
{
	return ticks * 1000000000UL / MTIME_FREQ;
}

static bool timespec_is_valid(const struct timespec *ts)
{
	return ts && ts->tv_sec >= 0 && ts->tv_nsec >= 0 &&
	       ts->tv_nsec < NSEC_PER_SEC;
}

static int timespec_compare(const struct timespec *left,
			    const struct timespec *right)
{
	if (left->tv_sec != right->tv_sec)
		return left->tv_sec < right->tv_sec ? -1 : 1;
	if (left->tv_nsec != right->tv_nsec)
		return left->tv_nsec < right->tv_nsec ? -1 : 1;
	return 0;
}

static void timespec_subtract_nonnegative(const struct timespec *left,
					  const struct timespec *right,
					  struct timespec *result)
{
	result->tv_sec = left->tv_sec - right->tv_sec;
	result->tv_nsec = left->tv_nsec - right->tv_nsec;
	if (result->tv_nsec < 0) {
		result->tv_nsec += NSEC_PER_SEC;
		result->tv_sec--;
	}
}

static bool timeval_is_zero(const struct timeval *tv)
{
	return tv->tv_sec == 0 && tv->tv_usec == 0;
}

static bool itimerval_value_is_zero(const struct itimerval *value)
{
	return timeval_is_zero(&value->it_value);
}

static bool timespec_is_zero(const struct timespec *ts)
{
	return ts->tv_sec == 0 && ts->tv_nsec == 0;
}

static bool itimerspec_value_is_zero(const struct itimerspec *value)
{
	return timespec_is_zero(&value->it_value);
}

static int timeval_to_mtime_delta(const struct timeval *tv, uint64_t *delta)
{
	uint64_t sec_ticks;
	uint64_t usec_ticks;

	if (!tv || !delta)
		return -EINVAL;
	if (tv->tv_sec < 0 || tv->tv_usec < 0 ||
	    tv->tv_usec >= (long)USEC_PER_SEC)
		return -EINVAL;

	if ((uint64_t)tv->tv_sec > UINT64_MAX / MTIME_FREQ)
		sec_ticks = UINT64_MAX;
	else
		sec_ticks = (uint64_t)tv->tv_sec * MTIME_FREQ;

	usec_ticks = ((uint64_t)tv->tv_usec * MTIME_FREQ + USEC_PER_SEC - 1) /
		     USEC_PER_SEC;
	if (usec_ticks > UINT64_MAX - sec_ticks)
		*delta = UINT64_MAX;
	else
		*delta = sec_ticks + usec_ticks;

	return 0;
}

static void mtime_to_timeval(uint64_t ticks, struct timeval *tv)
{
	uint64_t sec = ticks / MTIME_FREQ;
	uint64_t rem = ticks % MTIME_FREQ;
	uint64_t usec;

	usec = (rem * USEC_PER_SEC + MTIME_FREQ - 1) / MTIME_FREQ;
	if (usec >= USEC_PER_SEC) {
		sec++;
		usec -= USEC_PER_SEC;
	}

	tv->tv_sec = (long)sec;
	tv->tv_usec = (long)usec;
}

static void ktimer_insert_locked(struct ktimer *timer)
{
	struct list_head *pos;

	list_for_each (pos, &ktimer_queue.entries) {
		struct ktimer *queued = list_entry(pos, struct ktimer, node);

		if (timer->expires < queued->expires) {
			__list_add(&timer->node, pos->prev, pos);
			return;
		}
	}

	list_add_tail(&timer->node, &ktimer_queue.entries);
}

static uint64_t ktimer_next_interval_deadline(uint64_t expires,
					      uint64_t interval, uint64_t now)
{
	uint64_t next = mtime_deadline_after(expires, interval);

	while (next <= now && next != UINT64_MAX)
		next = mtime_deadline_after(next, interval);
	return next;
}

static struct ktimer *ktimer_detach_first_expired_locked(uint64_t now)
{
	struct ktimer *timer;

	if (list_empty(&ktimer_queue.entries))
		return NULL;

	timer = list_first_entry(&ktimer_queue.entries, struct ktimer, node);
	if (timer->expires > now)
		return NULL;

	list_del_init(&timer->node);
	timer->active = false;
	return timer;
}

static void itimer_real_fire(struct ktimer *timer, void *arg)
{
	struct itimer_state *state =
		container_of(timer, struct itimer_state, timer);
	struct task_struct *target;
	bool ref;
	bool state_ref = false;
	irq_flags_t flags;
	bool active;

	(void)arg;

	spin_lock_irqsave(&state->lock, &flags);
	target = state->target;
	ref = target && task_try_get(target);
	active = ktimer_active(timer);
	if (!active) {
		state->value = (struct itimerval){0};
		state->target = NULL;
		state_ref = state->target_ref;
		state->target_ref = false;
	}
	spin_unlock_irqrestore(&state->lock, flags);

	if (state_ref)
		task_put(target);
	if (ref) {
		(void)send_signal(SIGALRM, target);
		task_put(target);
	}
}

static void itimer_snapshot_locked(struct itimer_state *state,
				   struct itimerval *value, uint64_t now)
{
	itimer_state_value(state, value);
	if (ktimer_active(&state->timer))
		mtime_to_timeval(ktimer_remaining(&state->timer, now),
				 &value->it_value);
	else
		value->it_value = (struct timeval){0};
}

static void posix_timer_note_overrun(struct posix_timer *timer)
{
	if (timer->overrun < INT32_MAX)
		timer->overrun++;
}

static void posix_timer_fire(struct ktimer *timer, void *arg)
{
	struct posix_timer *posix_timer =
		container_of(timer, struct posix_timer, timer);
	struct posix_timer_table *table;
	struct task_struct *target = NULL;
	bool ref = false;
	irq_flags_t flags;
	int notify = SIGEV_NONE;
	int signo = 0;
	siginfo_t info = {0};

	(void)timer;
	(void)arg;

	if (!posix_timer->signal)
		return;

	table = &posix_timer->signal->posix_timers;
	spin_lock_irqsave(&table->lock, &flags);
	if (posix_timer->allocated) {
		target = posix_timer->target;
		notify = posix_timer->notify;
		ref = target && notify == SIGEV_SIGNAL ? task_try_get(target) : false;
		signo = posix_timer->signo;
		info.si_signo = signo;
		info.si_code = SI_TIMER;
		info.si_tid = posix_timer->id;
		info.si_overrun = posix_timer->overrun;
		info.si_int = posix_timer->sigev_value.sival_int;
		if (notify == SIGEV_SIGNAL && target &&
		    (task_pending_mask(target) & signal_mask(signo))) {
			posix_timer_note_overrun(posix_timer);
			notify = SIGEV_NONE;
		}
	}
	spin_unlock_irqrestore(&table->lock, flags);

	if (notify == SIGEV_SIGNAL && ref) {
		(void)send_signal_info(signo, &info, target);
		task_put(target);
	}
}

static int posix_timer_event_init(struct posix_timer *timer,
				  const sigevent_t *event)
{
	timer->sigev_value = (sigval_t){0};
	timer->notify = SIGEV_SIGNAL;
	timer->signo = SIGALRM;

	if (!event)
		return 0;

	switch (event->sigev_notify) {
	case SIGEV_NONE:
		timer->notify = SIGEV_NONE;
		timer->signo = 0;
		timer->sigev_value = event->sigev_value;
		return 0;
	case SIGEV_SIGNAL:
		if (!signal_is_valid(event->sigev_signo))
			return -EINVAL;
		timer->notify = SIGEV_SIGNAL;
		timer->signo = event->sigev_signo;
		timer->sigev_value = event->sigev_value;
		return 0;
	default:
		return -EINVAL;
	}
}

static void posix_timer_slot_reset(struct posix_timer *timer, timer_t id)
{
	memset(timer, 0, sizeof(*timer));
	ktimer_init(&timer->timer, posix_timer_fire, NULL);
	timer->id = id;
}

static struct posix_timer *
posix_timer_lookup_locked(struct posix_timer_table *table, timer_t id)
{
	if (!posix_timer_id_valid(id) || !test_bit(table->allocated, id))
		return NULL;
	return table->timers[id];
}

static void posix_timer_get(struct posix_timer *timer)
{
	BUG_ON(!timer);
	BUG_ON(!refcount_inc_not_zero(&timer->refs));
}

static void posix_timer_put(struct posix_timer *timer)
{
	if (!timer)
		return;
	if (!refcount_dec_and_test(&timer->refs))
		return;

	BUG_ON(timer->allocated);
	BUG_ON(timer->signal);
	BUG_ON(timer->target_ref);
	kfree(timer);
}

static void posix_timer_snapshot_locked(struct posix_timer *timer,
					struct itimerspec *value, uint64_t now)
{
	*value = timer->value;
	if (ktimer_active(&timer->timer))
		mtime_to_timespec(ktimer_remaining(&timer->timer, now),
				  &value->it_value);
	else
		value->it_value = (struct timespec){0};
}

static void posix_timer_detach_locked(struct posix_timer_table *table,
				      struct posix_timer *timer)
{
	timer->allocated = false;
	timer->signal = NULL;
	table->timers[timer->id] = NULL;
	clr_bit(table->allocated, timer->id);
}

void mtime_to_timespec(uint64_t ticks, struct timespec *ts)
{
	uint64_t sec = ticks / MTIME_FREQ;
	uint64_t rem = ticks % MTIME_FREQ;

	ts->tv_sec = (int64_t)sec;
	ts->tv_nsec = (int64_t)nsec_from_mtime_remainder(rem);
}

void kernel_realtime_now(struct timespec *value)
{
	struct timespec monotonic;
	struct timespec offset;
	irq_flags_t flags;
	int64_t seconds;
	int64_t nanoseconds;

	mtime_to_timespec(timer_now(), &monotonic);
	spin_lock_irqsave(&realtime_clock.lock, &flags);
	offset = realtime_clock.offset;
	spin_unlock_irqrestore(&realtime_clock.lock, flags);

	if (check_add_overflow(monotonic.tv_sec, offset.tv_sec, &seconds))
		goto saturated;
	nanoseconds = monotonic.tv_nsec + offset.tv_nsec;
	if (nanoseconds >= NSEC_PER_SEC) {
		nanoseconds -= NSEC_PER_SEC;
		if (check_add_overflow(seconds, 1LL, &seconds))
			goto saturated;
	}

	value->tv_sec = seconds;
	value->tv_nsec = nanoseconds;
	return;

saturated:
	value->tv_sec = INT64_MAX;
	value->tv_nsec = NSEC_PER_SEC - 1;
}

int kernel_realtime_set(const struct timespec *value)
{
	struct timespec monotonic;
	struct timespec offset;
	irq_flags_t flags;

	if (!timespec_is_valid(value))
		return -EINVAL;

	mtime_to_timespec(timer_now(), &monotonic);
	if (timespec_compare(value, &monotonic) < 0)
		return -EINVAL;

	timespec_subtract_nonnegative(value, &monotonic, &offset);
	spin_lock_irqsave(&realtime_clock.lock, &flags);
	realtime_clock.offset = offset;
	spin_unlock_irqrestore(&realtime_clock.lock, flags);
	return 0;
}

int timespec_to_mtime_delta(const struct timespec *ts, uint64_t *delta)
{
	uint64_t sec_ticks;
	uint64_t nsec_ticks;

	if (!ts || !delta)
		return -EINVAL;
	if (!timespec_is_valid(ts))
		return -EINVAL;

	if ((uint64_t)ts->tv_sec > UINT64_MAX / MTIME_FREQ)
		sec_ticks = UINT64_MAX;
	else
		sec_ticks = (uint64_t)ts->tv_sec * MTIME_FREQ;

	nsec_ticks = ((uint64_t)ts->tv_nsec * MTIME_FREQ + 999999999ULL) /
		     1000000000ULL;
	if (nsec_ticks > UINT64_MAX - sec_ticks)
		*delta = UINT64_MAX;
	else
		*delta = sec_ticks + nsec_ticks;

	return 0;
}

uint64_t mtime_deadline_after(uint64_t now, uint64_t delta)
{
	if (delta > UINT64_MAX - now)
		return UINT64_MAX;
	return now + delta;
}

int mtime_deadline_from_timespec(const struct timespec *ts,
				 struct wait_deadline *deadline)
{
	uint64_t delta;
	int ret;

	*deadline = wait_deadline_none();
	if (!ts)
		return 0;

	ret = timespec_to_mtime_delta(ts, &delta);
	if (ret < 0)
		return ret;

	*deadline = wait_deadline_at(mtime_deadline_after(timer_now(), delta));
	return 0;
}

int mtime_deadline_from_ms(long timeout_ms, struct wait_deadline *deadline)
{
	uint64_t delta;
	uint64_t ms;

	*deadline = wait_deadline_none();
	if (timeout_ms < 0)
		return 0;

	if (timeout_ms == 0) {
		*deadline = wait_deadline_at(timer_now());
		return 0;
	}

	ms = (uint64_t)timeout_ms;
	if (ms > (UINT64_MAX - 999ULL) / MTIME_FREQ)
		delta = UINT64_MAX;
	else
		delta = (ms * MTIME_FREQ + 999ULL) / 1000ULL;

	*deadline = wait_deadline_at(mtime_deadline_after(timer_now(), delta));
	return 0;
}

void ktimer_init(struct ktimer *timer, ktimer_fn_t function, void *arg)
{
	INIT_LIST_HEAD(&timer->node);
	INIT_LIST_HEAD(&timer->cancel_waiters);
	timer->function = function;
	timer->arg = arg;
	timer->expires = 0;
	timer->interval = 0;
	timer->active = false;
	timer->callback_running = false;
	timer->callback_task = NULL;
}

int ktimer_arm(struct ktimer *timer, uint64_t expires, uint64_t interval)
{
	irq_flags_t flags;

	spin_lock_irqsave(&ktimer_queue.lock, &flags);
	if (timer->active)
		list_del_init(&timer->node);
	timer->expires = expires;
	timer->interval = interval;
	timer->active = true;
	ktimer_insert_locked(timer);
	spin_unlock_irqrestore(&ktimer_queue.lock, flags);
	return 0;
}

bool ktimer_cancel(struct ktimer *timer)
{
	irq_flags_t flags;
	bool active;

	spin_lock_irqsave(&ktimer_queue.lock, &flags);
	active = timer->active;
	if (timer->active) {
		list_del_init(&timer->node);
		timer->active = false;
	}
	spin_unlock_irqrestore(&ktimer_queue.lock, flags);
	return active;
}

int ktimer_cancel_sync(struct ktimer *timer)
{
	struct ktimer_cancel_waiter waiter;
	bool irq_enabled;

	if (!in_task_context() || !preemptible() || spinlock_held())
		return -EINVAL;

	INIT_LIST_HEAD(&waiter.node);
	waiter.task = current_task();
	waiter.queued = false;
	waiter.ref = false;
	irq_enabled = !irqs_disabled();
	for (;;) {
		irq_flags_t flags;
		bool callback_running;
		bool callback_self;

		spin_lock_irqsave(&ktimer_queue.lock, &flags);
		if (timer->active) {
			list_del_init(&timer->node);
			timer->active = false;
		}
		callback_running = timer->callback_running;
		callback_self = callback_running &&
				timer->callback_task == current_task();
		if (callback_running && !callback_self && !waiter.queued) {
			BUG_ON(!task_try_get(waiter.task));
			waiter.ref = true;
			waiter.queued = true;
			list_add_tail(&waiter.node, &timer->cancel_waiters);
		}
		if (!callback_running && waiter.queued) {
			list_del_init(&waiter.node);
			waiter.queued = false;
		}
		spin_unlock_irqrestore(&ktimer_queue.lock, flags);

		if (!callback_running) {
			if (waiter.ref) {
				task_put(waiter.task);
				waiter.ref = false;
			}
			return 0;
		}
		if (callback_self)
			return -EDEADLK;

		if (!irq_enabled)
			local_irq_enable();
		if (sched_has_runnable())
			schedule();
		else
			wait_for_interrupt();
		if (!irq_enabled)
			local_irq_disable();
	}
}

void ktimer_run_expired(uint64_t now)
{
	irq_flags_t flags;

	for (;;) {
		struct ktimer *timer;
		ktimer_fn_t function;
		void *arg;
		LIST_HEAD(waiters);

		spin_lock_irqsave(&ktimer_queue.lock, &flags);
		timer = ktimer_detach_first_expired_locked(now);
		if (!timer) {
			spin_unlock_irqrestore(&ktimer_queue.lock, flags);
			break;
		}

		function = timer->function;
		arg = timer->arg;

		if (timer->interval != 0) {
			timer->expires = ktimer_next_interval_deadline(
				timer->expires, timer->interval, now);
			timer->active = true;
			ktimer_insert_locked(timer);
		}
		timer->callback_running = true;
		timer->callback_task = current_task();

		spin_unlock_irqrestore(&ktimer_queue.lock, flags);
		if (function)
			function(timer, arg);

		spin_lock_irqsave(&ktimer_queue.lock, &flags);
		timer->callback_running = false;
		timer->callback_task = NULL;
		/* Detached waiters may release the timer after they are woken. */
		while (!list_empty(&timer->cancel_waiters))
			list_move_tail(timer->cancel_waiters.next, &waiters);
		spin_unlock_irqrestore(&ktimer_queue.lock, flags);
		ktimer_wake_waiters(&waiters);
	}
}

void itimer_state_init(struct itimer_state *state)
{
	mutex_init(&state->operation_lock);
	spin_lock_init(&state->lock);
	state->value = (struct itimerval){0};
	ktimer_init(&state->timer, itimer_real_fire, NULL);
	state->target = NULL;
	state->target_ref = false;
}

void itimer_state_destroy(struct itimer_state *state)
{
	struct task_struct *target;
	irq_flags_t flags;
	bool target_ref;
	int ret;

	mutex_lock(&state->operation_lock);
	spin_lock_irqsave(&state->lock, &flags);
	target = state->target;
	target_ref = state->target_ref;
	state->value = (struct itimerval){0};
	state->target = NULL;
	state->target_ref = false;
	spin_unlock_irqrestore(&state->lock, flags);

	ret = ktimer_cancel_sync(&state->timer);
	BUG_ON(ret < 0);
	if (target_ref)
		task_put(target);
	mutex_unlock(&state->operation_lock);
}

int itimer_get_value(struct itimer_state *state, struct itimerval *value)
{
	irq_flags_t flags;

	mutex_lock(&state->operation_lock);
	spin_lock_irqsave(&state->lock, &flags);
	itimer_snapshot_locked(state, value, timer_now());
	spin_unlock_irqrestore(&state->lock, flags);
	mutex_unlock(&state->operation_lock);
	return 0;
}

int itimer_set_real(struct itimer_state *state, struct task_struct *target,
		    const struct itimerval *new_value,
		    struct itimerval *old_value)
{
	uint64_t value_delta;
	uint64_t interval_delta;
	irq_flags_t flags;
	uint64_t now;
	struct task_struct *old_target;
	struct task_struct *new_target = NULL;
	bool old_target_ref;
	bool new_target_ref = false;
	int ret;

	ret = timeval_to_mtime_delta(&new_value->it_value, &value_delta);
	if (ret < 0)
		return ret;
	ret = timeval_to_mtime_delta(&new_value->it_interval, &interval_delta);
	if (ret < 0)
		return ret;
	if (!itimerval_value_is_zero(new_value)) {
		if (!task_try_get(target))
			return -ESRCH;
		new_target = target;
		new_target_ref = true;
	}

	mutex_lock(&state->operation_lock);
	spin_lock_irqsave(&state->lock, &flags);
	now = timer_now();
	if (old_value)
		itimer_snapshot_locked(state, old_value, now);
	old_target = state->target;
	old_target_ref = state->target_ref;
	state->value = (struct itimerval){0};
	state->target = NULL;
	state->target_ref = false;
	spin_unlock_irqrestore(&state->lock, flags);

	ret = ktimer_cancel_sync(&state->timer);
	BUG_ON(ret < 0);
	if (old_target_ref)
		task_put(old_target);

	if (itimerval_value_is_zero(new_value))
		goto out;

	spin_lock_irqsave(&state->lock, &flags);
	state->value = *new_value;
	state->target = new_target;
	state->target_ref = new_target_ref;
	now = timer_now();
	ret = ktimer_arm(&state->timer, mtime_deadline_after(now, value_delta),
			 interval_delta);
	spin_unlock_irqrestore(&state->lock, flags);
	if (ret < 0) {
		spin_lock_irqsave(&state->lock, &flags);
		state->value = (struct itimerval){0};
		state->target = NULL;
		state->target_ref = false;
		spin_unlock_irqrestore(&state->lock, flags);
		if (new_target_ref) {
			task_put(new_target);
			new_target_ref = false;
		}
	}

out:
	mutex_unlock(&state->operation_lock);
	if (itimerval_value_is_zero(new_value) && new_target_ref)
		task_put(new_target);
	return ret;
}

void posix_timer_table_init(struct posix_timer_table *table)
{
	spin_lock_init(&table->lock);
	table->allocated = 0;

	for (timer_t id = 0; id < POSIX_TIMER_COUNT; id++)
		table->timers[id] = NULL;
}

void posix_timer_table_clear(struct posix_timer_table *table)
{
	struct posix_timer *timers[POSIX_TIMER_COUNT];
	irq_flags_t flags;

	for (timer_t id = 0; id < POSIX_TIMER_COUNT; id++)
		timers[id] = NULL;

	spin_lock_irqsave(&table->lock, &flags);
	for (timer_t id = 0; id < POSIX_TIMER_COUNT; id++) {
		struct posix_timer *timer = table->timers[id];

		if (timer) {
			posix_timer_get(timer);
			posix_timer_detach_locked(table, timer);
			timers[id] = timer;
		}
	}
	table->allocated = 0;
	spin_unlock_irqrestore(&table->lock, flags);

	for (timer_t id = 0; id < POSIX_TIMER_COUNT; id++) {
		struct posix_timer *timer = timers[id];
		int ret;

		if (!timer)
			continue;
		ret = ktimer_cancel_sync(&timer->timer);
		BUG_ON(ret < 0);
		if (timer->target_ref)
			task_put(timer->target);
		timer->target_ref = false;
		posix_timer_put(timer);
		posix_timer_put(timer);
	}
}

void posix_timer_table_destroy(struct posix_timer_table *table)
{
	posix_timer_table_clear(table);
}

int posix_timer_create(struct signal_struct *signal, clockid_t clock_id,
		       timer_t *timerid, const sigevent_t *event,
		       struct task_struct *target)
{
	struct posix_timer scratch;
	struct posix_timer_table *table;
	struct posix_timer *timer;
	irq_flags_t flags;
	timer_t id;
	int ret;

	if (!target)
		return -EINVAL;
	if (!clock_id_supported(clock_id))
		return -EINVAL;

	ret = posix_timer_event_init(&scratch, event);
	if (ret < 0)
		return ret;
	if (!task_try_get(target))
		return -ESRCH;

	timer = kmalloc(sizeof(*timer), ALLOC_NOWAIT);
	if (!timer) {
		task_put(target);
		return -ENOMEM;
	}

	posix_timer_slot_reset(timer, -1);
	refcount_set(&timer->refs, 1);
	table = &signal->posix_timers;

	spin_lock_irqsave(&table->lock, &flags);
	id = ffz(table->allocated);
	if (!posix_timer_id_valid(id)) {
		spin_unlock_irqrestore(&table->lock, flags);
		task_put(target);
		posix_timer_put(timer);
		return -EAGAIN;
	}

	set_bit(table->allocated, id);
	table->timers[id] = timer;
	timer->id = id;
	timer->signal = signal;
	timer->target = target;
	timer->clock_id = clock_id;
	timer->sigev_value = scratch.sigev_value;
	if (!event)
		timer->sigev_value.sival_int = id;
	timer->notify = scratch.notify;
	timer->signo = scratch.signo;
	timer->allocated = true;
	timer->target_ref = true;
	*timerid = id;
	spin_unlock_irqrestore(&table->lock, flags);
	return 0;
}

int posix_timer_gettime(struct signal_struct *signal, timer_t id,
			struct itimerspec *value)
{
	struct posix_timer_table *table = &signal->posix_timers;
	struct posix_timer *timer;
	irq_flags_t flags;
	uint64_t now;

	spin_lock_irqsave(&table->lock, &flags);
	timer = posix_timer_lookup_locked(table, id);
	if (!timer) {
		spin_unlock_irqrestore(&table->lock, flags);
		return -EINVAL;
	}

	now = timer_now();
	posix_timer_snapshot_locked(timer, value, now);
	spin_unlock_irqrestore(&table->lock, flags);
	return 0;
}

int posix_timer_settime(struct signal_struct *signal, timer_t id, int flags,
			const struct itimerspec *new_value,
			struct itimerspec *old_value)
{
	struct posix_timer_table *table = &signal->posix_timers;
	struct posix_timer *timer;
	uint64_t value_delta;
	uint64_t interval_delta;
	irq_flags_t irq_flags;
	uint64_t expires;
	uint64_t now;
	int ret;

	if (flags & ~TIMER_ABSTIME)
		return -EINVAL;

	ret = timespec_to_mtime_delta(&new_value->it_value, &value_delta);
	if (ret < 0)
		return ret;
	ret = timespec_to_mtime_delta(&new_value->it_interval, &interval_delta);
	if (ret < 0)
		return ret;

	spin_lock_irqsave(&table->lock, &irq_flags);
	timer = posix_timer_lookup_locked(table, id);
	if (!timer) {
		spin_unlock_irqrestore(&table->lock, irq_flags);
		return -EINVAL;
	}
	if ((flags & TIMER_ABSTIME) && timer->clock_id == CLOCK_REALTIME) {
		spin_unlock_irqrestore(&table->lock, irq_flags);
		return -EINVAL;
	}
	posix_timer_get(timer);

	now = timer_now();
	if (old_value)
		posix_timer_snapshot_locked(timer, old_value, now);
	spin_unlock_irqrestore(&table->lock, irq_flags);

	ret = ktimer_cancel_sync(&timer->timer);
	BUG_ON(ret < 0);

	spin_lock_irqsave(&table->lock, &irq_flags);
	if (posix_timer_lookup_locked(table, id) != timer) {
		spin_unlock_irqrestore(&table->lock, irq_flags);
		posix_timer_put(timer);
		return -EINVAL;
	}
	if (itimerspec_value_is_zero(new_value)) {
		timer->value = (struct itimerspec){0};
		timer->overrun = 0;
		spin_unlock_irqrestore(&table->lock, irq_flags);
		posix_timer_put(timer);
		return 0;
	}

	now = timer_now();
	timer->value = *new_value;
	timer->overrun = 0;
	if (flags & TIMER_ABSTIME)
		expires = value_delta;
	else
		expires = mtime_deadline_after(now, value_delta);
	ret = ktimer_arm(&timer->timer, expires, interval_delta);
	spin_unlock_irqrestore(&table->lock, irq_flags);
	posix_timer_put(timer);
	return ret;
}

int posix_timer_getoverrun(struct signal_struct *signal, timer_t id)
{
	struct posix_timer_table *table = &signal->posix_timers;
	struct posix_timer *timer;
	irq_flags_t flags;
	int overrun;

	spin_lock_irqsave(&table->lock, &flags);
	timer = posix_timer_lookup_locked(table, id);
	if (!timer) {
		spin_unlock_irqrestore(&table->lock, flags);
		return -EINVAL;
	}

	overrun = timer->overrun;
	spin_unlock_irqrestore(&table->lock, flags);
	return overrun;
}

int posix_timer_delete(struct signal_struct *signal, timer_t id)
{
	struct posix_timer_table *table = &signal->posix_timers;
	struct posix_timer *timer;
	irq_flags_t flags;

	spin_lock_irqsave(&table->lock, &flags);
	timer = posix_timer_lookup_locked(table, id);
	if (!timer) {
		spin_unlock_irqrestore(&table->lock, flags);
		return -EINVAL;
	}

	posix_timer_get(timer);
	posix_timer_detach_locked(table, timer);
	spin_unlock_irqrestore(&table->lock, flags);

	BUG_ON(ktimer_cancel_sync(&timer->timer) < 0);
	if (timer->target_ref) {
		task_put(timer->target);
		timer->target_ref = false;
	}
	posix_timer_put(timer);
	posix_timer_put(timer);
	return 0;
}
