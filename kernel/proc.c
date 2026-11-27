#include <nuvix/proc.h>
#include <nuvix/errno.h>
#include <nuvix/slab.h>
#include <nuvix/task.h>

struct proc *proc_alloc(struct task *leader)
{
    struct proc *proc = kzalloc(sizeof(*proc));

    if (!proc)
        return NULL;
    proc->leader = leader;
    refcount_set(&proc->refs, 1);
    spin_lock_init(&proc->lock);
    atomic_set(&proc->exiting, 0);
    INIT_LIST_HEAD(&proc->threads);
    INIT_LIST_HEAD(&proc->pending.list);
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
    flush_sigpending(&proc->pending, ~(sigset_t)0);
    kfree(proc);
}

int proc_add_thread(struct task *task)
{
    struct proc *proc = task->proc;

    spin_lock(&proc->lock);
    if (atomic_read(&proc->exiting)) {
        spin_unlock(&proc->lock);
        return -EINTR;
    }
    list_add_tail(&task->thread, &proc->threads);
    pid_publish(task);
    recalc_sigpending(task);
    spin_unlock(&proc->lock);
    return 0;
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
