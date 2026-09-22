#ifndef _NUVIX_MUTEX_H
#define _NUVIX_MUTEX_H

#include <nuvix/compiler.h>
#include <nuvix/spinlock.h>
#include <nuvix/types.h>
#include <nuvix/wait.h>

struct task_struct;

typedef struct {
	spinlock_t lock;
	struct task_struct *owner;
	struct wait_channel wait;
} mutex_t;

#define MUTEX_INIT(name) \
	{.lock = SPINLOCK_INIT, .owner = NULL, \
	 .wait = WAIT_CHANNEL_INIT((name).wait)}
#define DEFINE_MUTEX(name) mutex_t name = MUTEX_INIT(name)

static inline void mutex_init(mutex_t *mutex)
{
	spin_lock_init(&mutex->lock);
	mutex->owner = NULL;
	wait_channel_init(&mutex->wait);
}

/* Non-recursive, sleepable task context only; no FIFO ordering guarantee. */
__nonnull(1)
void mutex_lock(mutex_t *mutex);

/* Does not sleep; requires a non-idle task outside interrupt context. */
__must_check __nonnull(1)
bool mutex_trylock(mutex_t *mutex);

/* Only the owning task may unlock. */
__nonnull(1)
void mutex_unlock(mutex_t *mutex);

#endif
