/*
 * kernel/rwlock.c - sleeping reader/writer locks
 */

#include <nuvix/rwlock.h>
#include <nuvix/sched.h>
#include <nuvix/task.h>

__always_inline __must_check __pure
static inline bool rwlock_read_available(const rwlock_t *lock)
{
	return !lock->writer && lock->waiting_writers == 0;
}

__always_inline __must_check __pure
static inline bool rwlock_write_available(const rwlock_t *lock)
{
	return !lock->writer && lock->readers == 0 &&
	       lock->waiting_writers == 0;
}

__always_inline __must_check __pure
static inline bool rwlock_write_free(const rwlock_t *lock)
{
	return !lock->writer && lock->readers == 0;
}

bool rwlock_read_trylock(rwlock_t *lock)
{
	irq_flags_t flags;
	bool acquired = false;

	spin_lock_irqsave(&lock->state_lock, &flags);
	if (rwlock_read_available(lock)) {
		BUG_ON(lock->readers == UINT32_MAX);
		lock->readers++;
		acquired = true;
	}
	spin_unlock_irqrestore(&lock->state_lock, flags);

	return acquired;
}

void rwlock_read_lock(rwlock_t *lock)
{
	for (;;) {
		struct task_struct *task = current_task();
		struct task_wait *wait;
		const struct wait_deadline deadline = wait_deadline_none();
		wait_outcome_t outcome;
		irq_flags_t flags;
		int ret;
		bool acquired = false;

		spin_lock_irqsave(&lock->state_lock, &flags);
		if (rwlock_read_available(lock)) {
			BUG_ON(lock->readers == UINT32_MAX);
			lock->readers++;
			acquired = true;
		}
		spin_unlock_irqrestore(&lock->state_lock, flags);
		if (acquired)
			return;

		BUG_ON(!wait_context_can_sleep());
		wait = &task->wait;
		ret = wait_start(wait, 0, &deadline);
		BUG_ON(ret < 0);
		spin_lock_irqsave(&lock->state_lock, &flags);
		if (rwlock_read_available(lock)) {
			BUG_ON(lock->readers == UINT32_MAX);
			lock->readers++;
			acquired = true;
		} else {
			ret = wait_prepare(wait, &lock->wait, false);
		}
		spin_unlock_irqrestore(&lock->state_lock, flags);
		if (ret < 0) {
			wait_finish(wait);
			panic("rwlock: reader wait registration failed: %d", ret);
		}
		if (acquired) {
			wait_finish(wait);
			return;
		}
		ret = wait_block(wait, &outcome);
		wait_finish(wait);
		BUG_ON(ret < 0 || outcome != WAIT_OUTCOME_EVENT);
	}
}

void rwlock_read_unlock(rwlock_t *lock)
{
	irq_flags_t flags;
	bool wake_writer;
	bool wake_readers;

	spin_lock_irqsave(&lock->state_lock, &flags);
	BUG_ON(lock->readers == 0);
	lock->readers--;
	wake_writer = lock->readers == 0 && lock->waiting_writers != 0;
	wake_readers = lock->readers == 0 && !wake_writer;
	spin_unlock_irqrestore(&lock->state_lock, flags);

	if (wake_writer)
		wait_channel_wake_one(&lock->wait);
	else if (wake_readers)
		wait_channel_wake_all(&lock->wait);
}

bool rwlock_write_trylock(rwlock_t *lock)
{
	irq_flags_t flags;
	bool acquired = false;

	spin_lock_irqsave(&lock->state_lock, &flags);
	if (rwlock_write_available(lock)) {
		lock->writer = current_task();
		acquired = true;
	}
	spin_unlock_irqrestore(&lock->state_lock, flags);

	return acquired;
}

void rwlock_write_lock(rwlock_t *lock)
{
	for (;;) {
		struct task_struct *task = current_task();
		struct task_wait *wait;
		const struct wait_deadline deadline = wait_deadline_none();
		wait_outcome_t outcome;
		irq_flags_t flags;
		int ret;
		bool acquired = false;

		spin_lock_irqsave(&lock->state_lock, &flags);
		if (rwlock_write_available(lock)) {
			lock->writer = task;
			acquired = true;
		} else {
			/* Reserve the writer slot before sleeping so new readers
			 * cannot pass this writer between the two checks. */
			BUG_ON(lock->waiting_writers == UINT32_MAX);
			lock->waiting_writers++;
		}
		spin_unlock_irqrestore(&lock->state_lock, flags);
		if (acquired)
			return;

		BUG_ON(!wait_context_can_sleep());
		wait = &task->wait;
		ret = wait_start(wait, 0, &deadline);
		BUG_ON(ret < 0);
		spin_lock_irqsave(&lock->state_lock, &flags);
		if (rwlock_write_free(lock)) {
			lock->writer = task;
			BUG_ON(lock->waiting_writers == 0);
			lock->waiting_writers--;
			acquired = true;
		} else {
			ret = wait_prepare(wait, &lock->wait, true);
			if (ret < 0)
				lock->waiting_writers--;
		}
		spin_unlock_irqrestore(&lock->state_lock, flags);
		if (ret < 0) {
			wait_finish(wait);
			panic("rwlock: writer wait registration failed: %d", ret);
		}
		if (acquired) {
			wait_finish(wait);
			return;
		}
		ret = wait_block(wait, &outcome);
		wait_finish(wait);
		BUG_ON(ret < 0 || outcome != WAIT_OUTCOME_EVENT);

		/* Keep the writer reservation while handing ownership to this task. */
		spin_lock_irqsave(&lock->state_lock, &flags);
		BUG_ON(lock->waiting_writers == 0);
		if (rwlock_write_free(lock)) {
			lock->waiting_writers--;
			lock->writer = task;
			spin_unlock_irqrestore(&lock->state_lock, flags);
			return;
		}
		lock->waiting_writers--;
		spin_unlock_irqrestore(&lock->state_lock, flags);
	}
}

void rwlock_write_unlock(rwlock_t *lock)
{
	irq_flags_t flags;
	bool wake_writer;

	spin_lock_irqsave(&lock->state_lock, &flags);
	BUG_ON(lock->writer != current_task());
	lock->writer = NULL;
	wake_writer = lock->waiting_writers != 0;
	spin_unlock_irqrestore(&lock->state_lock, flags);

	if (wake_writer)
		wait_channel_wake_one(&lock->wait);
	else
		wait_channel_wake_all(&lock->wait);
}

void rwlock_downgrade_write(rwlock_t *lock)
{
	irq_flags_t flags;
	bool wake_readers;

	spin_lock_irqsave(&lock->state_lock, &flags);
	BUG_ON(lock->writer != current_task());
	BUG_ON(lock->readers != 0);
	lock->writer = NULL;
	lock->readers = 1;
	wake_readers = lock->waiting_writers == 0;
	spin_unlock_irqrestore(&lock->state_lock, flags);

	if (wake_readers)
		wait_channel_wake_all(&lock->wait);
}
