#include <nuvix/preempt.h>
#include <nuvix/task.h>

unsigned int preempt_count(void)
{
    return READ_ONCE(current->preempt_count);
}

void preempt_disable(void)
{
    unsigned int count = preempt_count();

    IFDEF(CONFIG_DEBUG_CONTEXT,
          Assert(count != (unsigned int)-1, "preempt count overflow\n"));
    WRITE_ONCE(current->preempt_count, count + 1);
    barrier();
}

void preempt_enable_no_resched(void)
{
    unsigned int count;

    barrier();
    count = preempt_count();
    IFDEF(CONFIG_DEBUG_CONTEXT, Assert(count, "preempt count underflow\n"));
    WRITE_ONCE(current->preempt_count, count - 1);
}

bool preemptible(void)
{
    return preempt_count() == 0 && !local_irqs_disabled();
}

void preempt_enable(void)
{
    preempt_enable_no_resched();
    if (preemptible() && task_need_resched(current))
        preempt_schedule();
}
