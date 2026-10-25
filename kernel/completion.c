#include <nuvix/completion.h>
#include <nuvix/timer.h>
#include <nuvix/tick.h>
#include <nuvix/errno.h>

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

static int wait_for_completion_state(struct completion *completion,
                                     enum task_state state)
{
    struct wait_entry wait;
    unsigned long flags;
    int ret = 0;

    might_sleep();
    wait_entry_init(&wait);
    for (;;) {
        preempt_disable();
        spin_lock_irqsave(&completion->wait.lock, flags);
        finish_wait_locked(&completion->wait, &wait);
        if (take_completion_locked(completion))
            break;
        if (state == TASK_SLEEP_INTERRUPTIBLE && signal_pending(current)) {
            ret = -ERESTARTSYS;
            break;
        }
        prepare_to_wait_locked(&completion->wait, &wait, state);
        spin_unlock_irqrestore(&completion->wait.lock, flags);
        sched_park();
    }
    spin_unlock_irqrestore(&completion->wait.lock, flags);
    preempt_enable();
    return ret;
}

void wait_for_completion(struct completion *completion)
{
    (void)wait_for_completion_state(completion, TASK_SLEEP_UNINTERRUPTIBLE);
}

int wait_for_completion_interruptible(struct completion *completion)
{
    return wait_for_completion_state(completion, TASK_SLEEP_INTERRUPTIBLE);
}

struct completion_timeout {
    struct timer timer;
    struct completion *completion;
    struct task *task;
};

static void completion_timed_out(struct timer *timer)
{
    struct completion_timeout *timeout =
        container_of(timer, struct completion_timeout, timer);
    unsigned long flags;

    /* Pair notification with the deadline check and sleep registration. */
    spin_lock_irqsave(&timeout->completion->wait.lock, flags);
    (void)sched_wake(timeout->task);
    spin_unlock_irqrestore(&timeout->completion->wait.lock, flags);
}

int wait_for_completion_until(struct completion *completion, uint64_t deadline)
{
    struct completion_timeout timeout;
    struct wait_entry wait;
    unsigned long flags;
    bool armed = false;
    int ret;

    might_sleep();
    timeout.completion = completion;
    timeout.task = current;
    timer_init(&timeout.timer, current_cpu_id(), completion_timed_out);
    wait_entry_init(&wait);
    for (;;) {
        preempt_disable();
        spin_lock_irqsave(&completion->wait.lock, flags);
        finish_wait_locked(&completion->wait, &wait);
        if (take_completion_locked(completion)) {
            ret = 0;
            break;
        }
        if (time_read() >= deadline) {
            ret = -ETIMEDOUT;
            break;
        }
        if (!armed) {
            ret = timer_start(&timeout.timer, deadline);
            if (ret)
                break;
            armed = true;
        }
        prepare_to_wait_locked(&completion->wait, &wait,
                               TASK_SLEEP_UNINTERRUPTIBLE);
        spin_unlock_irqrestore(&completion->wait.lock, flags);
        sched_park();
    }
    spin_unlock_irqrestore(&completion->wait.lock, flags);
    preempt_enable();
    if (armed)
        timer_cancel_sync(&timeout.timer);
    return ret;
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
