#include <nuvix/proc.h>
#include <nuvix/slab.h>
#include <nuvix/pid.h>
#include <nuvix/mm.h>
#include <nuvix/cleanup.h>
#include <nuvix/assert.h>

struct proc *proc_alloc(void)
{
    struct proc *proc __free(kfree) = kzalloc(sizeof(*proc));

    if (!proc)
        return NULL;
    int tgid __free(pid) = pid_alloc();

    if (tgid < 0)
        return NULL;
    struct mm_struct *mm __free(mm_put) = mm_alloc();

    if (!mm)
        return NULL;
    if (fdtable_init(&proc->files))
        return NULL;
    refcount_set(&proc->refcount, 1);
    spin_lock_init(&proc->state_lock);
    proc->tgid = move(tgid);
    proc->state = PROC_NORMAL;
    INIT_LIST_HEAD(&proc->members);
    INIT_LIST_HEAD(&proc->children);
    INIT_LIST_HEAD(&proc->sibling);
    proc->mm = move(mm);
    return move(proc);
}

void proc_get(struct proc *proc)
{
    refcount_inc(&proc->refcount);
}

void proc_put(struct proc *proc)
{
    if (!refcount_dec_and_test(&proc->refcount))
        return;
    Assert(!proc->nr_live && !proc->parent && list_empty(&proc->members) &&
               list_empty(&proc->children) && list_empty(&proc->sibling),
           "cannot destroy a published proc without process teardown\n");
    mm_put(proc->mm);
    fdtable_destroy(&proc->files);
    pid_free(proc->tgid);
    kfree(proc);
}
