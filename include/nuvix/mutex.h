#ifndef _NUVIX_MUTEX_H
#define _NUVIX_MUTEX_H

#include <nuvix/wait.h>

struct mutex {
    struct wait_queue wait;
    /* Protected with the waiters by wait.lock. */
    struct task *owner;
};

#define MUTEX_INITIALIZER(name)                                                \
    {.wait = WAIT_QUEUE_INITIALIZER((name).wait), .owner = NULL}
#define DEFINE_MUTEX(name) struct mutex name = MUTEX_INITIALIZER(name)

/* Exclusive initialization before use; cannot reinitialize an active mutex. */
void mutex_init(struct mutex *mutex);

/* Non-recursive, uninterruptible acquisition in ordinary sleepable context. */
void mutex_lock(struct mutex *mutex);
/* Thread context only; false when already owned, including by current. */
bool mutex_trylock(struct mutex *mutex);
/* Owner-only, outside hard IRQ; keep the mutex alive until this returns. */
void mutex_unlock(struct mutex *mutex);

#endif /* _NUVIX_MUTEX_H */
