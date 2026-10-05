#ifndef _NUVIX_SPINLOCK_H
#define _NUVIX_SPINLOCK_H

#include <nuvix/compiler.h>
#include <nuvix/irq.h>

/*
 * Temporary single-CPU implementation, valid only before scheduling exists.
 * Locks are non-recursive; nested locks must be released in reverse order.
 * IRQ masking provides no exclusion against other CPUs or synchronous traps.
 * Do not sleep or manually enable IRQs while holding a lock.
 */
typedef struct {
    unsigned long irq_flags;
} spinlock_t;

#define SPINLOCK_INITIALIZER  {.irq_flags = 0}
#define DEFINE_SPINLOCK(name) spinlock_t name = SPINLOCK_INITIALIZER

static inline void spin_lock_init(spinlock_t *lock)
{
    lock->irq_flags = 0;
}

static __always_inline unsigned long
__spin_lock_irqsave(spinlock_t *lock)
{
    unsigned long flags = local_irq_save();

    lock->irq_flags = flags;
    return flags;
}

static __always_inline void spin_lock(spinlock_t *lock)
{
    (void)__spin_lock_irqsave(lock);
}

static __always_inline void spin_unlock_irqrestore(spinlock_t *lock,
                                                          unsigned long flags)
{
    (void)lock;
    local_irq_restore(flags);
}

static __always_inline void spin_unlock(spinlock_t *lock)
{
    spin_unlock_irqrestore(lock, lock->irq_flags);
}

#define spin_lock_irqsave(lock, flags)                                         \
    do {                                                                       \
        (flags) = __spin_lock_irqsave(lock);                                   \
    } while (0)

#endif /* _NUVIX_SPINLOCK_H */
