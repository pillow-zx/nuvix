#ifndef _NUVIX_WAIT_H
#define _NUVIX_WAIT_H

#include <nuvix/sched.h>

struct wait_queue {
    spinlock_t lock;
    struct list_head waiters;
};

struct wait_entry {
    struct list_head node;
    struct task *task;
};

#define WAIT_QUEUE_INITIALIZER(name)                                           \
    {.lock = SPINLOCK_INITIALIZER, .waiters = LIST_INIT_HEAD((name).waiters)}
#define DEFINE_WAIT_QUEUE(name)                                                \
    struct wait_queue name = WAIT_QUEUE_INITIALIZER(name)

void wait_queue_init(struct wait_queue *queue);

static inline void wait_entry_init(struct wait_entry *entry)
{
    INIT_LIST_HEAD(&entry->node);
    entry->task = current;
}

/*
 * Current's stack entry; queue.lock held with IRQs off. Prepare also requires
 * the waiter's own preempt-disable level. Finish may cancel before park.
 */
void prepare_to_wait_locked(struct wait_queue *queue, struct wait_entry *entry);
void finish_wait_locked(struct wait_queue *queue, struct wait_entry *entry);

/* Publish the persistent condition before notifying; locked form needs IRQ off. */
void wake_up_all_locked(struct wait_queue *queue);
void wake_up_all(struct wait_queue *queue);

/*
 * Ordinary thread context, no caller locks or preempt-disable levels. Condition
 * is evaluated under queue.lock and must not sleep. The caller keeps the queue
 * and its condition object alive; producers protect the condition with this lock.
 */
#define wait_event(queue, condition)                                           \
    do {                                                                       \
        struct wait_queue *__wq = (queue);                                     \
        struct wait_entry __wait;                                              \
        unsigned long __flags;                                                 \
                                                                               \
        IFDEF(CONFIG_DEBUG_CONTEXT,                                            \
              Assert(preemptible() && !cpus[current_cpu_id()].irq_depth &&     \
                         current != this_rq()->idle,                           \
                     "invalid wait context\n"));                               \
        wait_entry_init(&__wait);                                              \
        for (;;) {                                                             \
            preempt_disable();                                                 \
            spin_lock_irqsave(&__wq->lock, __flags);                           \
            finish_wait_locked(__wq, &__wait);                                 \
            if (condition) {                                                   \
                spin_unlock_irqrestore(&__wq->lock, __flags);                  \
                preempt_enable();                                              \
                break;                                                         \
            }                                                                  \
            prepare_to_wait_locked(__wq, &__wait);                             \
            spin_unlock_irqrestore(&__wq->lock, __flags);                      \
            sched_park();                                                      \
        }                                                                      \
    } while (0)

#endif /* _NUVIX_WAIT_H */
