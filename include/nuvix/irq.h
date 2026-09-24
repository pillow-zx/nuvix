#ifndef _NUVIX_IRQ_H
#define _NUVIX_IRQ_H

#include <nuvix/cpu.h>
#include <nuvix/printk.h>
#include <arch/irq.h>

/* External IRQs use controller source IDs (1..DT riscv,ndev on RISC-V).
 * One handler and one fixed logical CPU own each source; no shared IRQs.
 * Handlers run in hard-IRQ context with local IRQs disabled. They must not
 * sleep or enable local IRQs, and must acknowledge their device before return.
 * The dispatcher owns controller claim/complete. */
typedef void (*irq_handler_t)(unsigned irq, void *data);

/* Register on an online CPU, initially disabled, with priority 1. The caller
 * owns handler/data until successful unregister and serializes registration,
 * synchronization and teardown for this source. No allocation is performed.
 * Invalid arguments return -EINVAL; an occupied source returns -EBUSY. */
int irq_register(unsigned irq, uint32_t cpu, irq_handler_t handler, void *data);

/* Idempotent, non-nesting enable/disable; usable from a handler. Disable
 * prevents new handler entries but does not wait for an active handler.
 * Hardware masking is deferred until its completion if a claim is active.
 * Enable during unregister returns -EBUSY; unregistered IRQs return -ENOENT. */
int irq_enable(unsigned irq);
int irq_disable(unsigned irq);

/* Both require a sleepable task context with no locks/resources needed by
 * the handler held. Disable the IRQ before synchronize if future entries
 * must be excluded. Unregister disables it and waits through controller
 * completion before detaching handler/data. A failed unregister retains the
 * registration (possibly disabled); it does not permit freeing data. */
int irq_synchronize(unsigned irq);
int irq_unregister(unsigned irq);

/* Registered sources only. Priority 0 suppresses delivery; unsupported
 * priorities return -EINVAL and leave the previous value in place. */
int irq_set_priority(unsigned irq, uint32_t priority);
/* Return a pending-bit snapshot (0/1), or a negative errno. A claimed IRQ
 * may still be executing while its pending bit is zero. */
int irq_pending(unsigned irq);

/**
 * @brief Return the current CPU's hard-IRQ handler nesting depth.
 *
 * This is read-only CPU-local state. It does not inspect or change hardware
 * IRQ state or preempt_count, and it is valid only on the current CPU.
 */
__must_check __pure
static inline uint32_t irq_nesting(void)
{
	return cpu_irq_nesting(current_cpu());
}

/**
 * @brief Return the current CPU's explicit preemption-disable depth.
 *
 * This query is independent from IRQ nesting and hardware IRQ state.
 */
__must_check __pure
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
__must_check __pure
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
__must_check __pure
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
static inline void irq_exit(void)
{
	struct cpu *cpu = current_cpu();

	BUG_ON(cpu_irq_nesting(cpu) == 0);
	cpu_dec_irq_nesting(cpu);
}

#endif
