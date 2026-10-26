#include <nuvix/mutex.h>

void mutex_init(struct mutex *mutex)
{
    wait_queue_init(&mutex->wait);
    mutex->owner = NULL;
}

static int mutex_lock_state(struct mutex *mutex, enum task_state state)
{
    struct wait_entry wait;
    unsigned long flags;
    int ret = 0;

    might_sleep();
    wait_entry_init(&wait);
    for (;;) {
        preempt_disable();
        spin_lock_irqsave(&mutex->wait.lock, flags);
        finish_wait_locked(&mutex->wait, &wait);
        IFDEF(CONFIG_DEBUG_LOCK, Assert(mutex->owner != current,
                                        "recursive mutex %p\n", (void *)mutex));
        if (!mutex->owner) {
            mutex->owner = current;
            break;
        }
        if (state == TASK_INTERRUPTIBLE && signal_pending(current)) {
            ret = -EINTR;
            break;
        }
        prepare_to_wait_locked(&mutex->wait, &wait, state);
        spin_unlock_irqrestore(&mutex->wait.lock, flags);
        sched_park();
    }
    spin_unlock_irqrestore(&mutex->wait.lock, flags);
    preempt_enable();
    return ret;
}

void mutex_lock(struct mutex *mutex)
{
    (void)mutex_lock_state(mutex, TASK_UNINTERRUPTIBLE);
}

int mutex_lock_interruptible(struct mutex *mutex)
{
    return mutex_lock_state(mutex, TASK_INTERRUPTIBLE);
}

bool mutex_trylock(struct mutex *mutex)
{
    unsigned long flags;
    bool acquired = false;

    IFDEF(CONFIG_DEBUG_CONTEXT, Assert(!cpus[current_cpu_id()].irq_depth,
                                       "mutex trylock in hard IRQ context\n"));
    spin_lock_irqsave(&mutex->wait.lock, flags);
    if (!mutex->owner) {
        mutex->owner = current;
        acquired = true;
    }
    spin_unlock_irqrestore(&mutex->wait.lock, flags);
    return acquired;
}

void mutex_unlock(struct mutex *mutex)
{
    unsigned long flags;

    IFDEF(CONFIG_DEBUG_CONTEXT, Assert(!cpus[current_cpu_id()].irq_depth,
                                       "mutex unlock in hard IRQ context\n"));
    spin_lock_irqsave(&mutex->wait.lock, flags);
    IFDEF(CONFIG_DEBUG_LOCK,
          Assert(mutex->owner == current, "mutex %p not held by current\n",
                 (void *)mutex));
    mutex->owner = NULL;
    wake_up_one_locked(&mutex->wait);
    spin_unlock_irqrestore(&mutex->wait.lock, flags);
}
