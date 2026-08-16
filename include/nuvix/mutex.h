#ifndef _NUVIX_MUTEX_H
#define _NUVIX_MUTEX_H

#include <nuvix/compiler.h>
#include <nuvix/printk.h>
#include <nuvix/spinlock.h>
#include <nuvix/types.h>
#include <nuvix/wait.h>

struct task_struct;

typedef struct {
	spinlock_t lock;
	struct task_struct *owner;
	struct wait_channel wait;
} mutex_t;

#define MUTEX_INIT(name, rank_value, irq_policy_value)                         \
	{                                                                      \
		.lock = SPINLOCK_INIT(rank_value, irq_policy_value),            \
		.owner = NULL,                                                 \
		.wait = WAIT_CHANNEL_INIT((name).wait),                        \
	}
#define DEFINE_MUTEX(name, rank_value, irq_policy_value)                       \
	mutex_t name = MUTEX_INIT(name, rank_value, irq_policy_value);

#define mutex_init(mutex, rank_value, irq_policy_value)                        \
	do {                                                                   \
		spin_lock_init(&(mutex)->lock, rank_value, irq_policy_value);   \
		(mutex)->owner = NULL;                                         \
		wait_channel_init(&(mutex)->wait);                             \
	} while (0)

__nonnull(1) __access_no_size(read_write, 1)
void mutex_lock(mutex_t *mutex);

__must_check __nonnull(1) __access_no_size(read_write, 1)
bool mutex_trylock(mutex_t *mutex);

__nonnull(1) __access_no_size(read_write, 1)
void mutex_unlock(mutex_t *mutex);

#endif
