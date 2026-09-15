#ifndef _NUVIX_SCHED_H
#define _NUVIX_SCHED_H

#include <nuvix/errno.h>
#include <nuvix/irq.h>
#include <nuvix/task.h>
#include <arch/processor.h>

#define SCHED_BOOT_AFFINITY_CPU 0u

enum sched_park_result {
	SCHED_PARK_INVALID = -EINVAL,
	SCHED_PARK_RACE,
	SCHED_PARKED,
};

void sched_init(void);
void sched_task_init(struct task_struct *task);
void sched_enqueue_new(struct task_struct *task);
void schedule(void);
void schedule_irqoff(void);
void sched_request(void);
void sched_tick(void);
void sched_yield(void);
bool sched_has_runnable(void);
void sched_idle(void);
__noreturn void sched_first_dispatch(struct task_struct *last);
__noreturn void sched_exit_current(void);

/* Current-only arm/finish; referenced remote notify is IRQ-safe. */
uint64_t sched_park_arm(struct task_struct *task,
			enum task_wait_policy policy, uint64_t signal_set);
void sched_park_finish(struct task_struct *task);
enum sched_park_result sched_block_current(uint64_t generation);
bool sched_wake(struct task_struct *task, uint64_t generation);
bool sched_wake_event(struct task_struct *task, uint64_t generation);
bool sched_wake_signal(struct task_struct *task, bool fatal);
bool sched_take_event(struct task_struct *task);
bool sched_wait_accepts_signal(struct task_struct *task, uint64_t mask);

/* Stop is current-only and must be serialized with group continue. */
bool sched_stop(struct task_struct *task);
bool sched_resume(struct task_struct *task);
bool sched_task_stopped(struct task_struct *task);
int sched_set_affinity(struct task_struct *task, const cpumask_t *requested);
cpumask_t sched_get_affinity(struct task_struct *task);
void sched_task_inherit_affinity(struct task_struct *child,
				struct task_struct *parent);
void sched_activate_mm(struct mm_struct *mm);

bool sched_retired_pop(struct task_struct **task);
void sched_notify_reaper(void);
void sched_reaper_sleep(void);

static inline void preempt_disable(void)
{
	cpu_inc_preempt_count(current_cpu());
}

static inline bool preemptible(void)
{
	return cpu_preempt_count(current_cpu()) == 0;
}

static inline bool sched_context_can_schedule(void)
{
	return current_task() && !in_irq() && preemptible() && !spinlock_held();
}

static inline void preempt_enable(void)
{
	cpu_dec_preempt_count(current_cpu());
	if (!irqs_disabled() && sched_context_can_schedule() &&
	    task_need_resched(current_task()))
		schedule();
}
#endif
