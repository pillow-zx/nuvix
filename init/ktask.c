#include <nuvix/task.h>
#include <nuvix/errno.h>
#include <nuvix/vfs.h>
#include <nuvix/macro.h>
#include <nuvix/sched.h>
#include <nuvix/list.h>
#include <nuvix/assert.h>
#include <nuvix/wait.h>
#include <nuvix/spinlock.h>

DEFINE_WAIT_QUEUE(reaper_wait);
bool reaper_pending;

__noreturn void ktask_exit(void)
{
    might_sleep();
    Assert(!current->parent, "kernel thread exit requires a kernel task\n");
    preempt_disable();
    sched_exit();
}

__noreturn void ktask_entry(struct task *prev, int (*fn)(void *), void *arg)
{
    sched_finish_switch(prev);
    preempt_enable_no_resched();
    local_irq_enable();

    (void)fn(arg);
    ktask_exit();
}

static int task_reaper(void *arg)
{
    (void)arg;
    for (;;) {
        unsigned long flags;

        wait_event(&reaper_wait, reaper_pending);
        spin_lock_irqsave(&reaper_wait.lock, flags);
        reaper_pending = false;
        spin_unlock_irqrestore(&reaper_wait.lock, flags);

        for (unsigned cpu_id = 0; cpu_id < nr_cpus; ++cpu_id) {
            struct rq *rq = cpu_rq(cpu_id);

            for (;;) {
                struct task *task;

                spin_lock_irqsave(&rq->lock, flags);
                if (list_empty(&rq->dead_list)) {
                    spin_unlock_irqrestore(&rq->lock, flags);
                    break;
                }
                task = list_first_entry(&rq->dead_list, struct task, rq_node);
                list_del_init(&task->rq_node);
                spin_unlock_irqrestore(&rq->lock, flags);
                task_reap(task);
            }
        }
    }
}

static int kernel_init(void *args)
{
    (void)args;
    int ret;

    ret = vfs_mount();
    if (ret)
        panic("FAT mount failed (%d)\n", ret);
    return 0;
}

int ktask_init(void)
{
    struct task *kinit __free(task_put) = NULL;
    struct task *kreaper __free(task_put) = NULL;

    kinit = task_alloc(false);
    if (!kinit)
        return -ENOMEM;
    arch_ktask_init(&kinit->ctx, kernel_init, NULL);
    sched_publish(kinit);

    kreaper = task_alloc(false);
    if (!kreaper)
        return -ENOMEM;
    arch_ktask_init(&kreaper->ctx, task_reaper, NULL);
    sched_publish(kreaper);

    return 0;
}
