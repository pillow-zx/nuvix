#include <kernel/mutex.h>
#include <kernel/sched.h>
#include <kernel/task.h>

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

	return locked;
}

void mutex_lock(mutex_t *mutex)
{
	for (;;) {
		struct task_struct *task = current_task();
		struct task_wait *wait;
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
			return;
		}

		BUG_ON(!wait_context_can_sleep());
		wait = &task->wait;
		ret = wait_start(wait, 0, &deadline);
		BUG_ON(ret < 0);
		spin_lock_irqsave(&mutex->lock, &flags);
		if (!mutex->owner) {
			mutex->owner = task;
			acquired = true;
		} else {
			ret = wait_prepare(wait, &mutex->wait, true);
		}
		spin_unlock_irqrestore(&mutex->lock, flags);
		if (ret < 0) {
			wait_finish(wait);
			BUG_ON(ret < 0);
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

void mutex_unlock(mutex_t *mutex)
{
	irq_flags_t flags;

	spin_lock_irqsave(&mutex->lock, &flags);
	BUG_ON(mutex->owner != current_task());

	mutex->owner = NULL;
	spin_unlock_irqrestore(&mutex->lock, flags);
	wait_channel_wake_one(&mutex->wait);
}
