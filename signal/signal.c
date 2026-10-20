#include <nuvix/signal.h>
#include <nuvix/task.h>
#include <nuvix/spinlock.h>
#include <nuvix/slab.h>

void signal_task_init(struct task *task)
{
    spin_lock_init(&task->signal_lock);
    INIT_LIST_HEAD(&task->pending.list);
    task->pending.signal = 0;
    task->blocked = 0;
    for (unsigned int i = 0; i < SIG_MAX; ++i)
        task->actions[i] = (struct sigaction){.sa_handler = SIG_DFL};
}

void signal_task_free(struct task *task)
{
    struct sigqueue *queue, *next;

    list_for_each_entry_safe (queue, next, &task->pending.list, list) {
        struct sigqueue *entry __free(kfree) = queue;

        list_del_init(&entry->list);
    }
    task->pending.signal = 0;
}
