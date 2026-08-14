/*
 * kernel/time.c - monotonic clock, realtime offset, and deadline conversion
 */

#include <nuvix/bitops.h>
#include <nuvix/bootinfo.h>
#include <nuvix/cpu.h>
#include <nuvix/errno.h>
#include <nuvix/sched.h>
#include <nuvix/spinlock.h>
#include <nuvix/time.h>
#include <nuvix/timer.h>
#include <nuvix/wait.h>

#define NSEC_PER_SEC 1000000000L

struct realtime_clock {
	spinlock_t lock;
	struct timespec offset;
};

struct clockevent_cpu {
	spinlock_t lock;
	uint64_t next_tick;
	uint64_t programmed;
	bool initialized;
};

static struct realtime_clock realtime_clock = {
	/* Task-context only, never nested with another lock; keep it ranked
	 * below the IRQ-safe block so a timer interrupt can preempt it. */
	.lock = SPINLOCK_INIT(LOCK_RANK_TOPOLOGY),
};

static struct clockevent_cpu clockevents[NR_CPUS];
/* Boot-health: set once per CPU after a local scheduler tick was handled. */
static atomic_isize_t clockevent_timer_seen[NR_CPUS];

bool cpu_timer_seen(uint32_t id)
{
	return atomic_isize_read_acquire(&clockevent_timer_seen[id]) != 0;
}

void clockevent_init(void)
{
	for (uint32_t id = 0; id < NR_CPUS; id++) {
		struct clockevent_cpu *event = &clockevents[id];

		/* Ranks with the deadline queue: the clockevent is programmed
		 * from deadline changes and consulted from the timer IRQ. */
		spin_lock_init(&event->lock, LOCK_RANK_DEADLINE);
		event->next_tick = UINT64_MAX;
		event->programmed = UINT64_MAX;
		event->initialized = false;
	}
}

void clockevent_cpu_init(void)
{
	struct clockevent_cpu *event = &clockevents[current_cpu()->id];
	uint64_t now = timer_now();

	event->next_tick = mtime_deadline_after(now, CLOCKS_PER_TICK);
	event->programmed = event->next_tick;
	event->initialized = true;
	timer_set(event->programmed);
}

void clockevent_deadline_changed(uint64_t expires)
{
	struct clockevent_cpu *event = &clockevents[current_cpu()->id];
	irq_flags_t flags;

	spin_lock_irqsave(&event->lock, &flags);
	if (event->initialized && expires < event->programmed) {
		event->programmed = expires;
		timer_set(expires);
	}
	spin_unlock_irqrestore(&event->lock, flags);
}

void clockevent_handle_irq(uint64_t now)
{
	struct clockevent_cpu *event = &clockevents[current_cpu()->id];
	uint64_t next_tick;
	uint64_t next_event;
	irq_flags_t flags;
	bool tick = false;

	wait_expire_deadlines(now);
	spin_lock_irqsave(&event->lock, &flags);
	BUG_ON(!event->initialized);
	if (now >= event->next_tick) {
		tick = true;
		do {
			event->next_tick = mtime_deadline_after(
				event->next_tick, CLOCKS_PER_TICK);
		} while (event->next_tick <= now &&
			 event->next_tick != UINT64_MAX);
	}
	next_tick = event->next_tick;
	spin_unlock_irqrestore(&event->lock, flags);

	if (tick)
		sched_tick();
	next_event = wait_next_deadline(next_tick);

	spin_lock_irqsave(&event->lock, &flags);
	event->programmed = next_event;
	timer_set(next_event);
	spin_unlock_irqrestore(&event->lock, flags);

	/* Boot-health: publish the first handled local scheduler tick after
	 * reprogramming the next timer, proving this CPU's Sstc path works
	 * end to end. Logical CPU 0 reads this only through cpu_timer_seen(). */
	if (tick)
		atomic_isize_set_release(&clockevent_timer_seen[current_cpu()->id],
					 1);
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

static uint64_t nsec_from_mtime_remainder(uint64_t ticks)
{
	return ticks * NSEC_PER_SEC / MTIME_FREQ;
}

void mtime_to_timespec(uint64_t ticks, struct timespec *ts)
{
	uint64_t seconds = ticks / MTIME_FREQ;
	uint64_t remainder = ticks % MTIME_FREQ;

	ts->tv_sec = (int64_t)seconds;
	ts->tv_nsec = (int64_t)nsec_from_mtime_remainder(remainder);
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
	uint64_t seconds;
	uint64_t nanoseconds;

	if (!ts || !delta || !timespec_is_valid(ts))
		return -EINVAL;

	if ((uint64_t)ts->tv_sec > UINT64_MAX / MTIME_FREQ)
		seconds = UINT64_MAX;
	else
		seconds = (uint64_t)ts->tv_sec * MTIME_FREQ;

	nanoseconds = ((uint64_t)ts->tv_nsec * MTIME_FREQ +
			       NSEC_PER_SEC - 1) / NSEC_PER_SEC;
	if (nanoseconds > UINT64_MAX - seconds)
		*delta = UINT64_MAX;
	else
		*delta = seconds + nanoseconds;
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

int mtime_deadline_from_ms(long timeout_ms,
				   struct wait_deadline *deadline)
{
	uint64_t delta;
	uint64_t milliseconds;

	*deadline = wait_deadline_none();
	if (timeout_ms < 0)
		return 0;
	if (timeout_ms == 0) {
		*deadline = wait_deadline_at(timer_now());
		return 0;
	}

	milliseconds = (uint64_t)timeout_ms;
	if (milliseconds > (UINT64_MAX - 999ULL) / MTIME_FREQ)
		delta = UINT64_MAX;
	else
		delta = (milliseconds * MTIME_FREQ + 999ULL) / 1000ULL;

	*deadline = wait_deadline_at(mtime_deadline_after(timer_now(), delta));
	return 0;
}

/* Phase-A boot banner: both rows are compile-time timer constants. */
BOOTINFO_BLOCK(timer, void,
	BROW("Timer Device", "sstc @ %llu MHz",
	     (unsigned long long)(MTIME_FREQ / 1000000ULL));
	BROW("Timer Tick", "%llu Hz", (unsigned long long)HZ);
)
