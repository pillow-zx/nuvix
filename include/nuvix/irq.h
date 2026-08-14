#ifndef _NUVIX_IRQ_H
#define _NUVIX_IRQ_H

#include <nuvix/cpu.h>
#include <nuvix/printk.h>
#include <arch/irq.h>

/**
 * @brief Return the current CPU's hard-IRQ handler nesting depth.
 *
 * This is read-only CPU-local state. It does not inspect or change hardware
 * IRQ state or preempt_count, and it is valid only on the current CPU.
 */
__always_inline __must_check __pure
static inline uint32_t irq_nesting(void)
{
	return cpu_irq_nesting(current_cpu());
}

/**
 * @brief Return the current CPU's explicit preemption-disable depth.
 *
 * This query is independent from IRQ nesting and hardware IRQ state.
 */
__always_inline __must_check __pure
static inline int preempt_count(void)
{
	return cpu_preempt_count(current_cpu());
}

/**
 * @brief Test whether execution is inside a hard-IRQ handler.
 *
 * The query has no side effects, cannot block, and does not imply that local
 * IRQs are disabled or that preemption is disabled.
 */
__always_inline __must_check __pure
static inline bool in_irq(void)
{
	return irq_nesting() != 0;
}

/**
 * @brief Test whether execution belongs to a non-idle task context.
 *
 * IRQ-disabled task execution remains task context. Hard-IRQ nesting is the
 * only IRQ condition excluded here; preempt_count and hardware IRQ state are
 * intentionally queried independently.
 */
__always_inline __must_check __pure
static inline bool in_task_context(void)
{
	struct cpu *cpu = current_cpu();
	struct task_struct *task = cpu_current_task(cpu);

	return task && task != cpu_idle_task(cpu) && !in_irq();
}

/**
 * @brief Enter one level of hard-IRQ handler context.
 *
 * The caller must run on the current CPU and pair this call with irq_exit().
 * Nesting is permitted. This helper neither changes hardware IRQ enable state
 * nor preempt_count, cannot sleep, allocate, migrate, or acquire locks, and
 * BUG_ONs if the nesting counter would overflow.
 */
__always_inline
static inline void irq_enter(void)
{
	struct cpu *cpu = current_cpu();

	BUG_ON(cpu_irq_nesting(cpu) == UINT32_MAX);
	cpu_inc_irq_nesting(cpu);
}

/**
 * @brief Leave one level of hard-IRQ handler context.
 *
 * The caller must run on the current CPU and have a matching irq_enter().
 * This helper neither changes hardware IRQ enable state nor preempt_count,
 * cannot sleep, allocate, migrate, or acquire locks, and BUG_ONs on underflow.
 */
__always_inline
static inline void irq_exit(void)
{
	struct cpu *cpu = current_cpu();

	BUG_ON(cpu_irq_nesting(cpu) == 0);
	cpu_dec_irq_nesting(cpu);
}

#endif
