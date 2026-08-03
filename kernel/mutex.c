#include <kernel/mutex.h>
#include <kernel/sched.h>
#include <kernel/task.h>

static int mutex_lock_probe(struct wait_session *session, void *arg)
{
	mutex_t *mutex = arg;
	irq_flags_t flags;
	int ret;

	spin_lock_irqsave(&mutex->lock, &flags);
	if (!mutex->owner) {
		mutex->owner = current_task();
		spin_unlock_irqrestore(&mutex->lock, flags);
		return 1;
	}

	ret = wait_session_watch(session, &mutex->wait);
	spin_unlock_irqrestore(&mutex->lock, flags);
	return ret;
}

void mutex_init(mutex_t *mutex)
{
	spin_lock_init(&mutex->lock);
	mutex->owner = NULL;
	wait_channel_init(&mutex->wait);
}

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
	struct wait_request source = {
		.kind = WAIT_KIND_MUTEX,
		.check = mutex_lock_probe,
		.arg = mutex,
		.channel_limit = 1,
	};
	int ret;

	if (mutex_trylock(mutex))
		return;

	ret = wait_event_uninterruptible(&source);
	BUG_ON(ret < 0);
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
