#include <nuvix/proc.h>
#include <nuvix/slab.h>
#include <nuvix/task.h>

struct proc *proc_alloc(void)
{
    struct proc *proc = kzalloc(sizeof(*proc));

    if (!proc)
        return NULL;
    refcount_set(&proc->refs, 1);
    spin_lock_init(&proc->lock);
    atomic_set(&proc->exiting, 0);
    INIT_LIST_HEAD(&proc->threads);
    INIT_LIST_HEAD(&proc->children);
    INIT_LIST_HEAD(&proc->sibling);
    return proc;
}

void proc_get(struct proc *proc)
{
    refcount_inc(&proc->refs);
}

void proc_put(struct proc *proc)
{
    if (!proc || !refcount_dec_and_test(&proc->refs))
        return;
    for (enum pid_type type = PIDTYPE_TGID; type < PIDTYPE_MAX; ++type)
        pid_put(proc->pids[type]);
    kfree(proc);
}

void proc_add_thread_locked(struct task *task)
{
    struct proc *proc = task->proc;

    assert_spin_locked(&proc->lock);
    list_add_tail(&task->thread, &proc->threads);
}

bool proc_remove_thread(struct task *task)
{
    struct proc *proc = task->proc;
    bool empty;

    spin_lock(&proc->lock);
    list_del_init(&task->thread);
    empty = list_empty(&proc->threads);
    spin_unlock(&proc->lock);
    return empty;
}
