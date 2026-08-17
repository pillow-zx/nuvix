#ifndef _NUVIX_SCHED_H
#define _NUVIX_SCHED_H

/**
 * @file sched.h
 * @brief Scheduler entry points and preemption counters.
 */

#include <nuvix/list.h>
#include <nuvix/task.h>
#include <nuvix/irq.h>
#include <nuvix/spinlock.h>
#include <arch/processor.h>

/*
 * Boot baseline: every task is affine to logical CPU 0 until
 * sched_set_affinity replaces the mask. The configured CPU count is fixed at
 * boot.
 */
#define SCHED_BOOT_AFFINITY_MASK BIT(0)

/**
 * @brief Initialize scheduler queues and policy state.
 */
void sched_init(void);

/**
 * @brief Switch from current task to the next runnable task.
 *
 * The caller must be in a task context, with no held spinlock, a zero
 * preemption-disable depth, and local IRQs enabled. IRQ-disabled callers must
 * use schedule_irqoff(). Invalid callers trigger a diagnostic.
 */
void schedule(void);

/** Scheduler/interrupt-return entry with local IRQs already disabled. */
void schedule_irqoff(void);

/**
 * @brief Switch task contexts and activate the next address space.
 *
 * The scheduler owns this seam. Callers must have local IRQs disabled and
 * hold no spinlock; the function does not establish these preconditions.
 */
__nonnull(1, 2)
void task_switch(struct task_struct *prev, struct task_struct *next);

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
void sched_enqueue_new(struct task_struct *task);

bool sched_has_runnable(void);

/** Block the current task for one wait generation. */
int sched_block_current(struct task_wait *wait);

/** Wake a task only when it still belongs to the supplied wait generation. */
bool sched_wake(struct task_struct *task, uint64_t generation);

/** Wake a task waiting on a scheduler-owned completion channel. */
bool sched_wake_external(struct task_struct *task);

int sched_set_affinity(struct task_struct *task, uint64_t mask);

uint64_t sched_get_affinity(struct task_struct *task);

/** Transfer one retired task to the independent reaper. */
__must_check
bool sched_retired_pop(struct task_struct **task);

/** Leave the current task forever after its task/proc cleanup is complete. */
__noreturn
void sched_exit_current(void);

/**
 * @brief Remove a task from its runqueue.
 * @param task Task that may currently be queued.
 */
void sched_dequeue(struct task_struct *task);

/** Move a stopped task back to the runnable state. */
bool sched_resume(struct task_struct *task);

/** Mark a live task stopped and remove it from its runqueue. */
bool sched_stop(struct task_struct *task);

__always_inline
static inline void preempt_disable(void)
{
	cpu_inc_preempt_count(current_cpu());
}

__always_inline __pure
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
	return current_task() && !in_irq() && preemptible() && !spinlock_held();
}

/**
 * @brief Re-enable preemption and consume a safe deferred reschedule.
 */
static inline void preempt_enable(void)
{
	struct task_struct *task = current_task();

	cpu_dec_preempt_count(current_cpu());
	if (preemptible() && !irqs_disabled() && task &&
	    task_need_resched(task) && sched_context_can_schedule())
		schedule();
}

#endif
