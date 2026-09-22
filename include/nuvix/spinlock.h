#ifndef _NUVIX_SPINLOCK_H
#define _NUVIX_SPINLOCK_H

#include <nuvix/atomic.h>
#include <nuvix/compiler.h>
#include <nuvix/irq.h>
#include <nuvix/printk.h>
#include <nuvix/types.h>

typedef struct spinlock {
	/* Zero is unlocked; debug builds encode the owning CPU as id + 1. */
	atomic_t locked;
} spinlock_t;

#define SPINLOCK_INIT {.locked = ATOMIC_INIT(0)}
#define DEFINE_SPINLOCK(name) spinlock_t name = SPINLOCK_INIT

static inline void spin_lock_init(spinlock_t *lock)
{
	atomic_set(&lock->locked, 0);
}

/* This count is runtime state, independent of debug configuration. */
__must_check __pure
static inline bool spinlock_held(void)
{
	return lock_depth() != 0;
}

static inline int spinlock_owner_value(void)
{
#ifdef CONFIG_DEBUG_CONTEXT
	return current_cpu()->id + 1;
#else
	return 1;
#endif
}

/* The caller has disabled preemption. IRQs may remain enabled while spinning. */
static inline void spinlock_acquire(spinlock_t *lock)
{
	int owner = spinlock_owner_value();

	for (;;) {
		int expected = 0;
		if (atomic_try_cmpxchg_acquire(&lock->locked, &expected, owner))
			break;
		IFDEF(CONFIG_DEBUG_CONTEXT,
		      BUG_ON(expected == owner);)
		if (NR_CPUS == 1)
			panic("spinlock deadlock: lock=%p caller=%p",
			      lock, __return_address());
	}
	irq_flags_t flags = local_irq_save();
	BUG_ON(current_cpu()->lock_depth == UINT32_MAX);
	current_cpu()->lock_depth++;
	local_irq_restore(flags);
}

/* Caller keeps IRQs disabled until ownership and CPU counters are updated. */
static inline void spinlock_release(spinlock_t *lock)
{
	struct cpu *cpu = current_cpu();

	BUG_ON(atomic_read(&lock->locked) != spinlock_owner_value());
	BUG_ON(cpu->lock_depth == 0 || cpu_preempt_count(cpu) <= 0);
	atomic_store_order(&lock->locked, 0, ATOMIC_ORDER_RELEASE);
	cpu->lock_depth--;
	cpu_dec_preempt_count(cpu);
}

static inline irq_flags_t __spin_lock_irqsave(spinlock_t *lock)
{
	irq_flags_t flags = local_irq_save();

	cpu_inc_preempt_count(current_cpu());
	spinlock_acquire(lock);
	return flags;
}

#define spin_lock_irqsave(lock, flags) \
	do { (flags) = __spin_lock_irqsave(lock); } while (0)

static inline void spin_unlock_irqrestore(spinlock_t *lock, irq_flags_t flags)
{
	local_irq_disable();
	spinlock_release(lock);
	local_irq_restore(flags);
}

/* Plain locks preserve IRQ state. Use irqsave when sharing with a handler. */
static inline void spin_lock(spinlock_t *lock)
{
	irq_flags_t flags = local_irq_save();

	cpu_inc_preempt_count(current_cpu());
	local_irq_restore(flags);
	spinlock_acquire(lock);
}

static inline void spin_unlock(spinlock_t *lock)
{
	irq_flags_t flags = local_irq_save();

	spinlock_release(lock);
	local_irq_restore(flags);
}

#endif
