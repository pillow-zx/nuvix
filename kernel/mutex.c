#include <nuvix/errno.h>
#include <nuvix/mutex.h>
#include <nuvix/printk.h>
#include <nuvix/sched.h>
#include <nuvix/task.h>

bool mutex_trylock(mutex_t *mutex)
{
	irq_flags_t flags;
	bool locked = false;

	spin_lock_irqsave(&mutex->lock, &flags);
	if (!mutex->owner) {
		mutex->owner = current_task();
		locked = true;
	}
	spin_unlock_irqrestore(&mutex->lock, flags);
	if (locked)
		task_sleep_lock_acquire(mutex, mutex->semantic_rank,
					TASK_SLEEP_LOCK_MUTEX);

	return locked;
}

void mutex_lock(mutex_t *mutex)
{
	IFDEF(CONFIG_DEBUG_CONTEXT,
	      BUG_ON(current_task() && !task_is_idle(current_task()) &&
		     !wait_may_block());)
	for (;;) {
		struct task_struct *task = current_task();
		struct wait_scope scope __wait_scope = {};
		const struct wait_deadline deadline = wait_deadline_none();
		wait_outcome_t outcome;
		irq_flags_t flags;
		int ret;
		bool acquired = false;

		spin_lock_irqsave(&mutex->lock, &flags);
		if (!mutex->owner) {
			mutex->owner = task;
			acquired = true;
			ret = 0;
		}
		spin_unlock_irqrestore(&mutex->lock, flags);
		if (acquired) {
			task_sleep_lock_acquire(mutex, mutex->semantic_rank,
						TASK_SLEEP_LOCK_MUTEX);
			return;
		}

		BUG_ON(!wait_context_can_sleep());
		ret = wait_scope_begin(&scope, 0, &deadline);
		BUG_ON(ret < 0);
		spin_lock_irqsave(&mutex->lock, &flags);
		if (!mutex->owner) {
			mutex->owner = task;
			acquired = true;
		} else {
			ret = wait_scope_prepare(&scope, &mutex->wait, true);
		}
		spin_unlock_irqrestore(&mutex->lock, flags);
		if (ret < 0) {
			wait_scope_complete(&scope);
			BUG_ON(ret < 0);
		}
		if (acquired) {
			wait_scope_complete(&scope);
			task_sleep_lock_acquire(mutex, mutex->semantic_rank,
						TASK_SLEEP_LOCK_MUTEX);
			return;
		}
		ret = wait_scope_block(&scope, &outcome);
		wait_scope_complete(&scope);
		BUG_ON(ret < 0 || outcome != WAIT_OUTCOME_EVENT);
	}
}

void mutex_unlock(mutex_t *mutex)
{
	irq_flags_t flags;

	task_sleep_lock_release(mutex, TASK_SLEEP_LOCK_MUTEX);
	spin_lock_irqsave(&mutex->lock, &flags);
	BUG_ON(mutex->owner != current_task());

	mutex->owner = NULL;
	spin_unlock_irqrestore(&mutex->lock, flags);
	wait_channel_wake_one(&mutex->wait);
}
