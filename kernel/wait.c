#include <nuvix/wait.h>

void wait_queue_init(struct wait_queue *queue)
{
    spin_lock_init(&queue->lock);
    INIT_LIST_HEAD(&queue->waiters);
}

void prepare_to_wait_locked(struct wait_queue *queue, struct wait_entry *entry,
                            unsigned int state)
{
    struct rq *rq = this_rq();
    unsigned long flags;

    assert_spin_locked(&queue->lock);
    if (list_empty(&entry->node))
        list_add_tail(&entry->node, &queue->waiters);
    spin_lock_irqsave(&rq->lock, flags);
    sched_set_sleep_locked(rq, state);
    spin_unlock_irqrestore(&rq->lock, flags);
}

void finish_wait_locked(struct wait_queue *queue, struct wait_entry *entry)
{
    assert_spin_locked(&queue->lock);
    if (list_empty(&entry->node))
        return;
    (void)sched_wake(entry->task);
    list_del_init(&entry->node);
}

void wake_up_one_locked(struct wait_queue *queue)
{
    struct wait_entry *entry;

    assert_spin_locked(&queue->lock);
    list_for_each_entry (entry, &queue->waiters, node) {
        if (sched_wake(entry->task))
            break;
    }
}

void wake_up_one(struct wait_queue *queue)
{
    unsigned long flags;

    spin_lock_irqsave(&queue->lock, flags);
    wake_up_one_locked(queue);
    spin_unlock_irqrestore(&queue->lock, flags);
}

void wake_up_all_locked(struct wait_queue *queue)
{
    struct wait_entry *entry;

    assert_spin_locked(&queue->lock);
    list_for_each_entry (entry, &queue->waiters, node)
        (void)sched_wake(entry->task);
}

void wake_up_all(struct wait_queue *queue)
{
    unsigned long flags;

    spin_lock_irqsave(&queue->lock, flags);
    wake_up_all_locked(queue);
    spin_unlock_irqrestore(&queue->lock, flags);
}
