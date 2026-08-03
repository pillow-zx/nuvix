#ifndef _CUTEOS_KERNEL_SCHED_H
#define _CUTEOS_KERNEL_SCHED_H

/**
 * @file sched.h
 * @brief Scheduler entry points and preemption counters.
 */

#include <kernel/list.h>
#include <kernel/task.h>
#include <kernel/irq.h>
#include <kernel/spinlock.h>

/**
 * @brief Initialize scheduler queues and policy state.
 */
void sched_init(void);

/**
 * @brief Switch from current task to the next runnable task.
 *
 * The caller must be in a task context, with no held spinlock and a zero
 * preemption-disable depth. Local IRQs may be enabled or disabled; the
 * scheduler preserves the entry state. Invalid callers trigger a diagnostic.
 */
void schedule(void);

/**
 * @brief Request a reschedule of the current task without switching.
 */
void sched_request(void);

/**
 * @brief Account one timer tick and request reschedule when needed.
 */
void sched_tick(void);

/**
 * @brief Voluntarily yield the CPU from the current task.
 */
void sched_yield(void);

/**
 * @brief Initialize scheduler fields in a new task.
 * @param task Task being initialized.
 */
__nonnull(1) __access_no_size(read_write, 1)
void sched_task_init(struct task_struct *task);

/**
 * @brief Insert a runnable task into the scheduler.
 * @param task Task in TASK_RUNNING state.
 */
__nonnull(1) __access_no_size(read_write, 1)
void sched_enqueue(struct task_struct *task);

/**
 * @brief Make a sleeping task runnable.
 * @param task Task to wake.
 */
__nonnull(1) __access_no_size(read_write, 1)
void sched_wakeup(struct task_struct *task);
bool sched_has_runnable(void);

/**
 * @brief Wake a specific task and enqueue it if needed.
 * @param task Task to wake.
 */
__nonnull(1) __access_no_size(read_write, 1)
void sched_wake_task(struct task_struct *task);

/**
 * @brief Remove a task from its runqueue.
 * @param task Task that may currently be queued.
 */
void sched_dequeue(struct task_struct *task);

static inline void preempt_disable(void)
{
	cpu_inc_preempt_count(current_cpu());
}

static inline bool preemptible(void)
{
	return cpu_preempt_count(current_cpu()) == 0;
}

/**
 * @brief Test whether the current context may enter the scheduler.
 *
 * This read-only guard describes the schedule() entry contract. It requires a
 * current task, no hard-IRQ context, no held spinlock, and a zero
 * preemption-disable depth. Local IRQ state is not part of this guard.
 */
static inline bool sched_context_can_schedule(void)
{
	return current_task() && !in_irq() &&
	       preemptible() && !spinlock_held();
}

/**
 * @brief Re-enable preemption and consume a safe deferred reschedule.
 */
static inline void preempt_enable(void)
{
	struct task_struct *task = current_task();

	cpu_dec_preempt_count(current_cpu());
	if (preemptible() && !irqs_disabled() && task &&
	    task_need_resched(task) &&
	    sched_context_can_schedule())
		schedule();
}

#ifdef KERNEL_SELFTEST
bool sched_test_runqueue_empty(void);
uint32_t sched_test_runnable_count(void);
struct task_struct *sched_test_peek_next(void);
void sched_test_force_boost(void);
uint8_t sched_test_level_slice(uint8_t level);
uint8_t sched_test_level(const struct task_struct *task);
uint8_t sched_test_time_slice(const struct task_struct *task);
uint8_t sched_test_ticks(const struct task_struct *task);
uint8_t sched_test_need_resched(const struct task_struct *task);
void sched_test_set_level(struct task_struct *task, uint8_t level);
void sched_test_set_budget(struct task_struct *task, uint8_t slice,
			   uint8_t ticks);
#endif

#endif
