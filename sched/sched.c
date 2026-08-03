#include <kernel/printk.h>
#include <kernel/rseq.h>

#include "internal.h"

void schedule(void)
{
	BUG_ON(in_irq());
	BUG_ON(!current_task());
	BUG_ON(spinlock_held());
	BUG_ON(!preemptible());

	struct task_struct *prev;
	struct task_struct *next;

	prev = current_task();
	task_set_need_resched(prev, 0);

	if (mlfq_empty()) {
		if (prev == &idle_task || prev->lifecycle.state == TASK_RUNNING)
			return;

		rseq_sched_switch(prev);
		task_switch_address_space(prev, &idle_task);
		set_current_task(&idle_task);
		task_switch(prev, &idle_task);
		return;
	}

	next = mlfq_pick_next();

	if (next == prev)
		return;

	if (prev != &idle_task && prev->lifecycle.state == TASK_RUNNING &&
	    list_empty(&prev->sched.run_list))
		mlfq_enqueue(prev);

	rseq_sched_switch(prev);
	set_current_task(next);
	task_switch_address_space(prev, next);
	task_switch(prev, next);
}

void sched_init(void)
{
	mlfq_init();
	pr_info("sched: MLFQ initialized (%d levels)\n", SCHED_MLFQ_LEVELS);
}

void sched_task_init(struct task_struct *task)
{
	mlfq_task_init(task);
}

void sched_enqueue(struct task_struct *task)
{
	mlfq_enqueue(task);
}

void sched_dequeue(struct task_struct *task)
{
	mlfq_dequeue(task);
}

void sched_wakeup(struct task_struct *task)
{
	mlfq_wakeup(task);
}

void sched_wake_task(struct task_struct *task)
{
	task_set_state(task, TASK_RUNNING);
	if (task != current_task())
		sched_wakeup(task);
}

bool sched_has_runnable(void)
{
	return !mlfq_empty();
}

static void sched_account_tick(void)
{
	struct task_struct *task = current_task();

	if (!task || task == &idle_task)
		return;

	if (task_trap_frome_user(task))
		task->cputime.utime_ticks++;
	else
		task->cputime.stime_ticks++;
}

void sched_tick(void)
{
	sched_account_tick();
	if (mlfq_tick())
		sched_request();
}

void sched_request(void)
{
	struct task_struct *task = current_task();

	if (task && task != &idle_task)
		task_set_need_resched(task, 1);
}

void sched_yield(void)
{
	struct task_struct *task = current_task();

	if (!task || task == &idle_task)
		return;

	sched_request();
	schedule();
}

#ifdef KERNEL_SELFTEST
bool sched_test_runqueue_empty(void)
{
	return mlfq_empty();
}

uint32_t sched_test_runnable_count(void)
{
	return mlfq_count();
}

struct task_struct *sched_test_peek_next(void)
{
	return mlfq_peek_next();
}

void sched_test_force_boost(void)
{
	mlfq_boost();
}

uint8_t sched_test_level_slice(uint8_t level)
{
	return mlfq_level_slice(level);
}

uint8_t sched_test_level(const struct task_struct *task)
{
	return task ? task->sched.sched_level : 0;
}

uint8_t sched_test_time_slice(const struct task_struct *task)
{
	return task ? task->sched.time_slice : 0;
}

uint8_t sched_test_ticks(const struct task_struct *task)
{
	return task ? task->sched.sched_ticks : 0;
}

uint8_t sched_test_need_resched(const struct task_struct *task)
{
	return task ? task->sched.need_resched : 0;
}

void sched_test_set_level(struct task_struct *task, uint8_t level)
{
	if (!task)
		return;

	BUG_ON(level >= SCHED_MLFQ_LEVELS);
	task->sched.sched_level = level;
	task->sched.time_slice = mlfq_level_slice(level);
}

void sched_test_set_budget(struct task_struct *task, uint8_t slice,
			   uint8_t ticks)
{
	if (!task)
		return;

	task->sched.time_slice = slice;
	task->sched.sched_ticks = ticks;
}
#endif
