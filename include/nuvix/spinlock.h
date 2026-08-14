#ifndef _NUVIX_SPINLOCK_H
#define _NUVIX_SPINLOCK_H

#include <nuvix/atomic.h>
#include <nuvix/compiler.h>
#include <nuvix/irq.h>
#include <nuvix/printk.h>
#include <nuvix/types.h>

typedef struct spinlock {
	atomic_t locked;
	IFDEF(CONFIG_DEBUG_CONTEXT, uint16_t rank;)
} spinlock_t;

/*
 * Lock ordering rules (must hold in every acquisition sequence):
 *
 *  1. Task context acquires locks in strictly increasing rank.  This makes
 *     the kernel-wide hold-wait graph a total order, so no deadlock cycle
 *     can exist.
 *  2. Task context and interrupt handlers on the same CPU share one lock
 *     stack (cpu->locks): the rank check below therefore also validates
 *     cross-context ordering.  Any lock acquired in hardirq context must
 *     rank at least as high as every lock that task context may hold with
 *     IRQs enabled.
 *  3. Consequently, task-context locks that hardirq handlers can reach
 *     (WAIT class and below) must be held with interrupts disabled via
 *     spin_lock_irqsave; DEADLINE and above form the "IRQ-safe" block.
 *  4. rank 0 = unranked, exempt from the check (gradual migration).
 *
 * Lock-held sections only detach objects and move references; they must not
 * execute a final put (refcount to zero), free, signal delivery, wake,
 * TTY hangup, user memory access, schedule, or any blocking operation.
 * Side effects run after the lock is released on a stable reference or
 * snapshot acquired inside the lock.
 */
#define LOCK_RANK_SESSION      10
#define LOCK_RANK_TTY	       11
#define LOCK_RANK_SIGNAL       15
#define LOCK_RANK_TOPOLOGY     20
#define LOCK_RANK_PID	       20
#define LOCK_RANK_PRINTK_READ  20
#define LOCK_RANK_PRINTK_RING  25
#define LOCK_RANK_VFS_MOUNT    25
#define LOCK_RANK_PAGE_CACHE   25
#define LOCK_RANK_CONSOLE_EMIT 26
#define LOCK_RANK_WAIT_CHANNEL 30
#define LOCK_RANK_ALLOC	       35
#define LOCK_RANK_WAIT	       40
#define LOCK_RANK_DEADLINE     45
#define LOCK_RANK_RUNQUEUE     50
#define LOCK_RANK_RETIRED      60

#define SPINLOCK_INIT(...)                                                     \
	{.locked = ATOMIC_INIT(0),                                             \
	 IFDEF(CONFIG_DEBUG_CONTEXT, __VA_OPT__(.rank = __VA_ARGS__, ))}
#define DEFINE_SPINLOCK(name, ...)                                             \
	spinlock_t name = SPINLOCK_INIT(__VA_ARGS__);

#define spin_lock_init(lock, ...)                                              \
	do {                                                                   \
		BUG_ON(!lock);                                                 \
		atomic_set(&(lock)->locked, 0);                                \
		IFDEF(CONFIG_DEBUG_CONTEXT, (lock)->rank = 0;                  \
		      __VA_OPT__((lock)->rank = __VA_ARGS__;))                 \
	} while (0)

/**
 * @brief Test whether the current CPU owns at least one spinlock.
 *
 * The query is available in every build and never allocates or changes
 * context state. Debug builds additionally make individual lock membership
 * queryable through spinlock_is_held_by_current().
 */
__always_inline __must_check __pure
static inline bool spinlock_held(void)
{
	return lock_depth() != 0;
}

/**
 * @brief Test whether a lock is recorded as held by the current CPU.
 *
 * The address set is a debug-only diagnostic. In a non-debug build there is
 * no membership information, so callers must use spinlock_held() instead.
 */
#ifdef CONFIG_DEBUG_CONTEXT
__always_inline __must_check __pure __nonnull(1)
static inline bool spinlock_held_by_current(const spinlock_t *lock)
{
	const struct cpu *cpu = current_cpu();

	for (uint32_t index = 0; index < cpu_lock_depth(cpu); index++)
		if (cpu->locks[index] == lock)
			return true;
	return false;
}
#endif

#ifdef CONFIG_DEBUG_CONTEXT
__always_inline __nonnull(1)
static inline void spinlock_track_acquire(spinlock_t *lock, irq_flags_t flags, bool irqsave)
{
	struct cpu *cpu = current_cpu();
	irq_flags_t track_flags = local_irq_save();

	uint32_t depth = cpu_lock_depth(cpu);

	BUG_ON(depth >= CPU_LOCK_MAX);
	if (depth && lock->rank && cpu->locks[depth - 1]->rank &&
	    lock->rank < cpu->locks[depth - 1]->rank)
		panic("spinlock rank inversion: lock=%p rank=%u top=%p rank=%u",
		      lock, lock->rank, cpu->locks[depth - 1],
		      cpu->locks[depth - 1]->rank);
	cpu->locks[depth] = lock;
	cpu->lock_flags[depth] = flags;
	cpu->lock_irqsave[depth] = irqsave;

	cpu->lock_depth++;
	local_irq_restore(track_flags);
}
#endif

#ifdef CONFIG_DEBUG_CONTEXT
__always_inline __nonnull(1)
static inline void spinlock_track_release(const spinlock_t *lock, irq_flags_t flags, bool irqsave)
{
	struct cpu *cpu = current_cpu();
	irq_flags_t track_flags = local_irq_save();
	uint32_t depth = cpu_lock_depth(cpu);

	BUG_ON(depth == 0);
	if (cpu->locks[depth - 1] != lock)
		panic("spinlock LIFO: release=%p top=%p depth=%u "
		      "stack=[%p,%p,%p,%p]",
		      lock, cpu->locks[depth - 1], depth,
		      depth > 0 ? cpu->locks[0] : NULL,
		      depth > 1 ? cpu->locks[1] : NULL,
		      depth > 2 ? cpu->locks[2] : NULL,
		      depth > 3 ? cpu->locks[3] : NULL);
	BUG_ON(cpu->lock_irqsave[depth - 1] != irqsave);
	BUG_ON(irqsave && cpu->lock_flags[depth - 1] != flags);
	cpu->locks[depth - 1] = NULL;
	cpu->lock_flags[depth - 1] = 0;
	cpu->lock_irqsave[depth - 1] = false;

	cpu->lock_depth = depth - 1;
	local_irq_restore(track_flags);
}
#endif

__always_inline
static inline void spin_lock_irqsave(spinlock_t *lock, irq_flags_t *flags)
{
	int expected;

	BUG_ON(!lock);
	BUG_ON(!flags);

#ifdef CONFIG_DEBUG_CONTEXT
	BUG_ON(spinlock_held_by_current(lock));
	BUG_ON(lock_depth() >= CPU_LOCK_MAX);
#endif

	cpu_inc_preempt_count(current_cpu());
	*flags = local_irq_save();
	do {
		expected = 0;
		if (!atomic_try_cmpxchg_acquire(&lock->locked, &expected, 1) &&
		    NR_CPUS == 1)
			panic("spinlock deadlock: lock=%p value=%d caller=%p "
			      "irq=%u",
			      lock, expected, __return_address(),
			      irq_nesting());
	} while (expected != 0);
	IFDEF(CONFIG_DEBUG_CONTEXT, spinlock_track_acquire(lock, *flags, true);)
}

__always_inline
static inline void spin_unlock_irqrestore(spinlock_t *lock, irq_flags_t flags)
{
	int old;

	BUG_ON(!lock);

	old = atomic_xchg_release(&lock->locked, 0);
	if (old != 1)
		panic("spinlock corrupt unlock: lock=%p value=%d caller=%p",
		      lock, old, __return_address());
	IFDEF(CONFIG_DEBUG_CONTEXT, spinlock_track_release(lock, flags, true);)
	local_irq_restore(flags);
	cpu_dec_preempt_count(current_cpu());
}

/**
 * @brief Acquire a lock without changing local IRQ state.
 *
 * Plain locks still disable kernel preemption.  Callers that can race with
 * an interrupt handler must use the irqsave variant instead.
 */
__always_inline
static inline void spin_lock(spinlock_t *lock)
{
	int expected;

	BUG_ON(!lock);
#ifdef CONFIG_DEBUG_CONTEXT
	BUG_ON(spinlock_held_by_current(lock));
	BUG_ON(lock_depth() >= CPU_LOCK_MAX);
#endif
	cpu_inc_preempt_count(current_cpu());
	do {
		expected = 0;
		if (!atomic_try_cmpxchg_acquire(&lock->locked, &expected, 1) &&
		    NR_CPUS == 1)
			panic("spinlock deadlock: lock=%p value=%d caller=%p "
			      "irq=%u",
			      lock, expected, __return_address(),
			      irq_nesting());
	} while (expected != 0);
	IFDEF(CONFIG_DEBUG_CONTEXT, spinlock_track_acquire(lock, 0, false);)
}

__always_inline
static inline void spin_unlock(spinlock_t *lock)
{
	int old;

	BUG_ON(!lock);
	old = atomic_xchg_release(&lock->locked, 0);
	if (old != 1)
		panic("spinlock corrupt unlock: lock=%p value=%d caller=%p",
		      lock, old, __return_address());
	IFDEF(CONFIG_DEBUG_CONTEXT, spinlock_track_release(lock, 0, false);)
	cpu_dec_preempt_count(current_cpu());
}

#endif
