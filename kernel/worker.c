/*
 * kernel/worker.c - small kernel worker helpers
 */

#include <kernel/time.h>
#include <kernel/timer.h>
#include <kernel/wait.h>
#include <kernel/worker.h>

static uint64_t worker_interval_ticks(unsigned int interval_sec)
{
	return (uint64_t)interval_sec * MTIME_FREQ;
}

void worker_run_periodic(unsigned int interval_sec, void (*work)(void *),
			 void *arg)
{
	uint64_t interval;

	if (!work || interval_sec == 0)
		return;

	interval = worker_interval_ticks(interval_sec);
	for (;;) {
		struct wait_deadline deadline = wait_deadline_at(
			mtime_deadline_after(timer_now(), interval));
		int ret;

		ret = wait_sleep_until(&deadline);
		if (ret < 0)
			return;
		work(arg);
	}
}
