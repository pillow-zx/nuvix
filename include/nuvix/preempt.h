#ifndef _NUVIX_PREEMPT_H
#define _NUVIX_PREEMPT_H

#include <nuvix/task.h>
#include <nuvix/irq.h>

#define INIT_PREEMPT_COUNT 1U

/* Ordinary reschedule entry: preempt_count == 0 and IRQs enabled. */
void preempt_schedule(void);

/* Only this CPU's execution context modifies current's count. */
static __always_inline unsigned int preempt_count(void)
{
    return READ_ONCE(current->preempt_count);
}

static __always_inline void preempt_disable(void)
{
    unsigned int count = preempt_count();

    IFDEF(CONFIG_DEBUG_CONTEXT,
          Assert(count != (unsigned int)-1, "preempt count overflow\n"));
    WRITE_ONCE(current->preempt_count, count + 1);
    barrier();
}

static __always_inline void preempt_enable_no_resched(void)
{
    unsigned int count;

    barrier();
    count = preempt_count();
    IFDEF(CONFIG_DEBUG_CONTEXT, Assert(count, "preempt count underflow\n"));
    WRITE_ONCE(current->preempt_count, count - 1);
}

static __always_inline bool preemptible(void)
{
    return preempt_count() == 0 && !local_irqs_disabled();
}

static __always_inline void preempt_enable(void)
{
    preempt_enable_no_resched();
    if (preemptible() && task_need_resched(current))
        preempt_schedule();
}

#endif /* _NUVIX_PREEMPT_H */
