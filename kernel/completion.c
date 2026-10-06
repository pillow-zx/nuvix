#include <nuvix/completion.h>

void init_completion(struct completion *completion)
{
    completion->done = 0;
    wait_queue_init(&completion->wait);
}

void reinit_completion(struct completion *completion)
{
    completion->done = 0;
}

static bool take_completion_locked(struct completion *completion)
{
    if (!completion->done)
        return false;
    if (completion->done != UINT32_MAX)
        --completion->done;
    return true;
}

void complete(struct completion *completion)
{
    unsigned long flags;

    spin_lock_irqsave(&completion->wait.lock, flags);
    if (completion->done != UINT32_MAX)
        ++completion->done;
    wake_up_one_locked(&completion->wait);
    spin_unlock_irqrestore(&completion->wait.lock, flags);
}

void complete_all(struct completion *completion)
{
    unsigned long flags;

    spin_lock_irqsave(&completion->wait.lock, flags);
    completion->done = UINT32_MAX;
    wake_up_all_locked(&completion->wait);
    spin_unlock_irqrestore(&completion->wait.lock, flags);
}

void wait_for_completion(struct completion *completion)
{
    struct wait_entry wait;
    unsigned long flags;

    might_sleep();
    wait_entry_init(&wait);
    for (;;) {
        preempt_disable();
        spin_lock_irqsave(&completion->wait.lock, flags);
        finish_wait_locked(&completion->wait, &wait);
        if (take_completion_locked(completion)) {
            spin_unlock_irqrestore(&completion->wait.lock, flags);
            preempt_enable();
            return;
        }
        prepare_to_wait_locked(&completion->wait, &wait);
        spin_unlock_irqrestore(&completion->wait.lock, flags);
        sched_park();
    }
}

bool try_wait_for_completion(struct completion *completion)
{
    unsigned long flags;
    bool completed;

    spin_lock_irqsave(&completion->wait.lock, flags);
    completed = take_completion_locked(completion);
    spin_unlock_irqrestore(&completion->wait.lock, flags);
    return completed;
}
