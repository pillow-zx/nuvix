#ifndef _NUVIX_SPINLOCK_H
#define _NUVIX_SPINLOCK_H

#include <nuvix/atomic.h>
#include <nuvix/compiler.h>
#include <nuvix/irq.h>
#include <nuvix/printk.h>
#include <nuvix/types.h>

enum spinlock_irq_policy {
	LOCK_IRQ_TASK_ONLY,
	LOCK_IRQ_HARDIRQ_REACHABLE,
};

typedef struct spinlock {
	atomic_t locked;
	IFDEF(CONFIG_DEBUG_CONTEXT,
	      uint16_t rank;
	      enum spinlock_irq_policy irq_policy;)
} spinlock_t;

#define LOCK_RANK_SESSION          10
#define LOCK_RANK_TTY              20
#define LOCK_RANK_SIGNAL_HAND      30
#define LOCK_RANK_SIGNAL_SHARED    40
#define LOCK_RANK_REALTIME_CLOCK   50
#define LOCK_RANK_VFORK            55
#define LOCK_RANK_TOPOLOGY         60
#define LOCK_RANK_PROC             65
#define LOCK_RANK_PID              70
#define LOCK_RANK_PRINTK_READ      80
#define LOCK_RANK_FILES_FDTABLE    90
#define LOCK_RANK_VFS_MOUNT        105
#define LOCK_RANK_FILES_FS         100
#define LOCK_RANK_FILE_POSITION    110
#define LOCK_RANK_VFS_CACHE        120
#define LOCK_RANK_INODE            130
#define LOCK_RANK_EXT2_SB          140
#define LOCK_RANK_MM_MMAP          150
#define LOCK_RANK_PRINTK_RING      160
#define LOCK_RANK_PAGE_CACHE       180
#define LOCK_RANK_CONSOLE_EMIT     190
#define LOCK_RANK_VIRTIO_SUBMIT    210
#define LOCK_RANK_EVENTPOLL        220
#define LOCK_RANK_PIPE             230
#define LOCK_RANK_FUTEX_BUCKET     240
#define LOCK_RANK_WAIT_CHANNEL     250
#define LOCK_RANK_ALLOC_BUDDY      260
#define LOCK_RANK_ALLOC_SLAB       270
#define LOCK_RANK_ALLOC_VMALLOC    280
#define LOCK_RANK_DEADLINE_QUEUE   290
#define LOCK_RANK_CLOCKEVENT       300
#define LOCK_RANK_WAIT             310
#define LOCK_RANK_RUNQUEUE         320
#define LOCK_RANK_RETIRED          330

#define SPINLOCK_INIT(rank_value, irq_policy_value)                            \
	{.locked = ATOMIC_INIT(0),                                             \
	 IFDEF(CONFIG_DEBUG_CONTEXT, .rank = (rank_value),                       \
	       .irq_policy = (irq_policy_value),)}
#define DEFINE_SPINLOCK(name, rank_value, irq_policy_value)                    \
	spinlock_t name = SPINLOCK_INIT(rank_value, irq_policy_value);

#define spin_lock_init(lock, rank_value, irq_policy_value)                     \
	do {                                                                   \
		BUG_ON(!lock);                                                 \
		atomic_set(&(lock)->locked, 0);                                \
		IFDEF(CONFIG_DEBUG_CONTEXT, (lock)->rank = (rank_value);        \
		      (lock)->irq_policy = (irq_policy_value);)                \
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
	bool hardirq = in_irq();
	bool irq_disabled = irqs_disabled();
	irq_flags_t track_flags = local_irq_save();

	uint32_t depth = cpu_lock_depth(cpu);

	BUG_ON(depth >= CPU_LOCK_MAX);
	if (lock->irq_policy == LOCK_IRQ_TASK_ONLY && hardirq)
		panic("task-only spinlock in hardirq: lock=%p rank=%u",
		      lock, lock->rank);
	if (lock->irq_policy == LOCK_IRQ_HARDIRQ_REACHABLE && !hardirq &&
	    !irq_disabled)
		panic("hardirq-reachable spinlock with IRQs enabled: lock=%p "
		      "rank=%u",
		      lock, lock->rank);
	if (depth && lock->rank && cpu->locks[depth - 1]->rank &&
	    lock->rank <= cpu->locks[depth - 1]->rank)
		panic("spinlock rank not strictly increasing: lock=%p rank=%u "
		      "top=%p rank=%u",
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

	IFDEF(CONFIG_DEBUG_CONTEXT, BUG_ON(spinlock_held_by_current(lock));)
	IFDEF(CONFIG_DEBUG_CONTEXT, BUG_ON(lock_depth() >= CPU_LOCK_MAX);)

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

	IFDEF(CONFIG_DEBUG_CONTEXT, BUG_ON(spinlock_held_by_current(lock));)
	IFDEF(CONFIG_DEBUG_CONTEXT, BUG_ON(lock_depth() >= CPU_LOCK_MAX);)


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
