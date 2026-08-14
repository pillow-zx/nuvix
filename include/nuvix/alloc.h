/**
 * @file alloc.h
 * @brief Allocation modes and allocator context contracts.
 */

#ifndef _NUVIX_ALLOC_H
#define _NUVIX_ALLOC_H

#include <nuvix/compiler.h>
#include <nuvix/irq.h>
#include <nuvix/spinlock.h>

/**
 * @enum alloc_mode
 * @brief Context permission for a kernel allocation.
 */
enum alloc_mode {
	ALLOC_NOWAIT,
	ALLOC_SLEEPABLE,
};

/**
 * @brief Test whether the current context may perform an allocation.
 *
 * NOWAIT allocations are allowed during bootstrap, in idle context, with
 * local IRQs disabled, or with preemption disabled, provided that execution
 * is not in hard IRQ context and no spinlock is held. SLEEPABLE allocations
 * require an IRQ-enabled, preemptible task context with no held spinlock.
 */
__always_inline __must_check __pure
static inline bool alloc_can_allocate(enum alloc_mode mode)
{
	if (mode != ALLOC_NOWAIT && mode != ALLOC_SLEEPABLE)
		return false;
	if (in_irq() || spinlock_held())
		return false;
	if (mode == ALLOC_NOWAIT)
		return true;

	return in_task_context() && preempt_count() == 0 && !irqs_disabled();
}

/**
 * @brief Test whether the current context may release an allocation.
 */
__always_inline __must_check __pure
static inline bool alloc_can_free(void)
{
	return !in_irq() && !spinlock_held();
}

/**
 * @brief Diagnose an invalid allocation context.
 */
__always_inline
static inline void alloc_check(enum alloc_mode mode)
{
	BUG_ON(!alloc_can_allocate(mode));
}

/**
 * @brief Diagnose an invalid free context.
 */
__always_inline
static inline void alloc_free_check(void)
{
	BUG_ON(!alloc_can_free());
}

#endif
