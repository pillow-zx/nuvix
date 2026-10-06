#include <nuvix/kthread.h>
#include <nuvix/preempt.h>
#include <nuvix/sched.h>

static __noreturn void kthread_entry(struct task *prev)
{
    int (*fn)(void *) = current->thread_fn;
    void *arg = current->thread_arg;

    sched_finish_switch(prev);
    preempt_enable_no_resched();
    local_irq_enable();

    (void)fn(arg);
    kthread_exit();
}

__noreturn void kthread_exit(void)
{
    might_sleep();
    Assert(!current->proc, "kernel thread exit requires a kernel task\n");
    preempt_disable();
    sched_exit();
}

struct task *create_kthread(int (*fn)(void *), void *arg)
{
    struct task *task __free(task_put) = task_alloc(NULL, false);

    if (!task)
        return NULL;
    task->thread_fn = fn;
    task->thread_arg = arg;
    task->ctx.ra = (unsigned long)kthread_entry;
    task->preempt_count = INIT_PREEMPT_COUNT;
    return move(task);
}
