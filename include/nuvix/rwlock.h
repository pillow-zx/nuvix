#ifndef _NUVIX_RWLOCK_H
#define _NUVIX_RWLOCK_H

#include <nuvix/compiler.h>
#include <nuvix/printk.h>
#include <nuvix/spinlock.h>
#include <nuvix/types.h>
#include <nuvix/wait.h>

struct task_struct;

/*
 * A sleeping, writer-preferred reader/writer lock. Readers may run
 * concurrently, while writers exclude both readers and other writers.
 * Recursive acquisition and read-to-write upgrade are not supported.
 *
 * The state-lock rank must be lower than LOCK_RANK_WAIT_CHANNEL because a
 * contended acquisition nests the state lock with the wait channel lock.
 * irq_policy_value must be LOCK_IRQ_TASK_ONLY: this is a sleeping lock and
 * its unlock path may acquire the task-only wait channel lock.
 */
typedef struct {
	spinlock_t state_lock;
	struct task_struct *writer;
	struct wait_channel wait;
	uint32_t readers;
	uint32_t waiting_writers;
} rwlock_t;

#define RWLOCK_INIT(name, rank_value, irq_policy_value)                         \
	{                                                                      \
		.state_lock = SPINLOCK_INIT(rank_value, irq_policy_value),       \
		.writer = NULL,                                                  \
		.wait = WAIT_CHANNEL_INIT((name).wait),                          \
		.readers = 0,                                                    \
		.waiting_writers = 0,                                            \
	}
#define DEFINE_RWLOCK(name, rank_value, irq_policy_value)                       \
	rwlock_t name = RWLOCK_INIT(name, rank_value, irq_policy_value);

#define rwlock_init(lock, rank_value, irq_policy_value)                         \
	do {                                                                   \
		spin_lock_init(&(lock)->state_lock, rank_value, irq_policy_value); \
		(lock)->writer = NULL;                                         \
		(lock)->readers = 0;                                           \
		(lock)->waiting_writers = 0;                                   \
		wait_channel_init(&(lock)->wait);                              \
	} while (0)

__nonnull(1) __access_no_size(read_write, 1)
void rwlock_read_lock(rwlock_t *lock);

__must_check __nonnull(1) __access_no_size(read_write, 1)
bool rwlock_read_trylock(rwlock_t *lock);

__nonnull(1) __access_no_size(read_write, 1)
void rwlock_read_unlock(rwlock_t *lock);

__nonnull(1) __access_no_size(read_write, 1)
void rwlock_write_lock(rwlock_t *lock);

__must_check __nonnull(1) __access_no_size(read_write, 1)
bool rwlock_write_trylock(rwlock_t *lock);

__nonnull(1) __access_no_size(read_write, 1)
void rwlock_write_unlock(rwlock_t *lock);

/** Convert the current writer ownership into one reader ownership. */
__nonnull(1) __access_no_size(read_write, 1)
void rwlock_downgrade_write(rwlock_t *lock);

#endif
