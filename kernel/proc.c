#include <nuvix/proc.h>
#include <nuvix/completion.h>
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
    atomic_set(&proc->state, PROC_NORMAL);
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

bool proc_finish_stop_locked(struct task *task)
{
    struct proc *proc = task->proc;

    assert_spin_locked(&proc->lock);
    if (!task_test_flag(task, TF_STOPPENDING))
        return false;
    Assert(proc->remaining, "pending stop requires a remaining thread\n");
    task_clear_flag(task, TF_STOPPENDING);
    return --proc->remaining == 0;
}

int proc_add_thread(struct task *task)
{
    struct proc *proc = task->proc;

    spin_lock(&proc->lock);
    if (atomic_read(&proc->state) == PROC_EXITING || proc->done) {
        spin_unlock(&proc->lock);
        return -EINTR;
    }
    if (atomic_read(&proc->state) == PROC_STOPPING) {
        task_set_flag(task, TF_STOPPENDING);
        ++proc->remaining;
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
    if (empty && atomic_read(&proc->state) != PROC_EXITING)
        proc->status = task->exit;
    if (proc->done && list_is_singular(&proc->threads))
        complete(proc->done);
    spin_unlock(&proc->lock);
    return empty;
}
