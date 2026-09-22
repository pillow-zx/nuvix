#include <nuvix/mutex.h>
#include <nuvix/task.h>

bool mutex_trylock(mutex_t *mutex)
{
	irq_flags_t flags;
	bool acquired = false;

	BUG_ON(!in_task_context());
	spin_lock_irqsave(&mutex->lock, flags);
	if (!mutex->owner) {
		mutex->owner = current_task();
		acquired = true;
	}
	spin_unlock_irqrestore(&mutex->lock, flags);
	return acquired;
}

void mutex_lock(mutex_t *mutex)
{
	struct task_struct *task = current_task();
	irq_flags_t flags;

	BUG_ON(!wait_may_block());
	spin_lock_irqsave(&mutex->lock, flags);
	BUG_ON(mutex->owner == task);
	if (!mutex->owner) {
		mutex->owner = task;
		spin_unlock_irqrestore(&mutex->lock, flags);
		return;
	}
	spin_unlock_irqrestore(&mutex->lock, flags);

	for (;;) {
		struct wait_scope scope __wait_scope = {};
		const struct wait_deadline deadline = wait_deadline_none();
		wait_outcome_t outcome;
		int ret = wait_scope_begin(&scope, 0, &deadline);

		BUG_ON(ret < 0);
		/* Check and enqueue under the same lock to avoid lost wakeups. */
		spin_lock_irqsave(&mutex->lock, flags);
		if (!mutex->owner) {
			mutex->owner = task;
			spin_unlock_irqrestore(&mutex->lock, flags);
			wait_scope_complete(&scope);
			return;
		}
		ret = wait_scope_prepare(&scope, &mutex->wait, true);
		spin_unlock_irqrestore(&mutex->lock, flags);
		BUG_ON(ret < 0);
		ret = wait_scope_block(&scope, &outcome);
		wait_scope_complete(&scope);
		BUG_ON(ret < 0 || outcome != WAIT_OUTCOME_EVENT);
	}
}

void mutex_unlock(mutex_t *mutex)
{
	irq_flags_t flags;

	BUG_ON(!in_task_context());
	spin_lock_irqsave(&mutex->lock, flags);
	BUG_ON(mutex->owner != current_task());
	mutex->owner = NULL;
	spin_unlock_irqrestore(&mutex->lock, flags);
	wait_channel_wake_one(&mutex->wait);
}
