#ifndef _NUVIX_SPINLOCK_H
#define _NUVIX_SPINLOCK_H

#include <nuvix/atomic.h>
#include <nuvix/cleanup.h>
#include <nuvix/preempt.h>

/*
 * Non-recursive TTAS locks. Ordinary operations account for preemption;
 * IRQ-shared locks require irqsave. Ordinary unlock restores preemption and
 * handles pending rescheduling once the context is preemptible.
 */
typedef struct {
    atomic_t locked;
    IFDEF(CONFIG_DEBUG_LOCK, struct task *owner);
} spinlock_t;

#define SPINLOCK_INITIALIZER  {.locked = ATOMIC_INIT(0)}
#define DEFINE_SPINLOCK(name) spinlock_t name = SPINLOCK_INITIALIZER

static inline void spin_lock_init(spinlock_t *lock)
{
    atomic_set(&lock->locked, 0);
    IFDEF(CONFIG_DEBUG_LOCK, atomic_ptr_set(&lock->owner, NULL));
}

static __always_inline void assert_spin_locked(const spinlock_t *lock)
{
    IFDEF(CONFIG_DEBUG_LOCK,
          Assert(atomic_read(&lock->locked) &&
                     atomic_ptr_read(&lock->owner) == current,
                 "spinlock %p not held by current\n", (void *)lock));
    (void)lock;
}

/* Physical lock only; the caller keeps its context pinned and accounts IRQs. */
static __always_inline void __spin_lock(spinlock_t *lock)
{
    IFDEF(CONFIG_DEBUG_LOCK, Assert(atomic_ptr_read(&lock->owner) != current,
                                    "recursive spinlock %p\n", (void *)lock));
    for (;;) {
        while (atomic_read(&lock->locked))
            ;
        if (!atomic_xchg_acquire(&lock->locked, 1))
            break;
    }
    IFDEF(CONFIG_DEBUG_LOCK, atomic_ptr_set(&lock->owner, current));
}

static __always_inline bool __spin_trylock(spinlock_t *lock)
{
    int32_t unlocked = 0;

    if (!atomic_try_cmpxchg_acquire(&lock->locked, &unlocked, 1))
        return false;
    IFDEF(CONFIG_DEBUG_LOCK, atomic_ptr_set(&lock->owner, current));
    return true;
}

static __always_inline void __spin_unlock(spinlock_t *lock)
{
    assert_spin_locked(lock);
    IFDEF(CONFIG_DEBUG_LOCK, atomic_ptr_set(&lock->owner, NULL));
    atomic_set_release(&lock->locked, 0);
}

/* Scheduler-only: IRQ off, physical lock retained across the task switch. */
static __always_inline void __spin_handoff(spinlock_t *lock, struct task *next)
{
    assert_spin_locked(lock);
    IFDEF(CONFIG_DEBUG_LOCK, atomic_ptr_set(&lock->owner, next));
    (void)next;
}

static __always_inline void spin_lock(spinlock_t *lock)
{
    preempt_disable();
    __spin_lock(lock);
}

static __always_inline void spin_unlock(spinlock_t *lock)
{
    __spin_unlock(lock);
    preempt_enable();
}

static __always_inline bool spin_trylock(spinlock_t *lock)
{
    preempt_disable();
    if (__spin_trylock(lock))
        return true;
    preempt_enable();
    return false;
}

static __always_inline unsigned long __spin_lock_irqsave(spinlock_t *lock)
{
    unsigned long flags = local_irq_save();

    spin_lock(lock);
    return flags;
}

static __always_inline void spin_unlock_irqrestore(spinlock_t *lock,
                                                   unsigned long flags)
{
    __spin_unlock(lock);
    local_irq_restore(flags);
    preempt_enable();
}

#define spin_lock_irqsave(lock, flags)                                         \
    do {                                                                       \
        (flags) = __spin_lock_irqsave(lock);                                   \
    } while (0)

DECLARE_GUARD(spinlock, spinlock_t *, spin_lock(state->value),
              spin_unlock(state->value))

DECLARE_GUARD(spinlock_irqsave, spinlock_t *,
              spin_lock_irqsave(state->value, state->flags),
              spin_unlock_irqrestore(state->value, state->flags),
              unsigned long flags)

#endif /* _NUVIX_SPINLOCK_H */
