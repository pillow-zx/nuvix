#include <nuvix/proc.h>
#include <nuvix/slab.h>
#include <nuvix/assert.h>

struct proc *proc_alloc(void)
{
    struct proc *proc = kzalloc(sizeof(*proc));

    if (!proc)
        return NULL;
    refcount_set(&proc->refcount, 1);
    spin_lock_init(&proc->state_lock);
    proc->state = PROC_NORMAL;
    INIT_LIST_HEAD(&proc->members);
    INIT_LIST_HEAD(&proc->children);
    INIT_LIST_HEAD(&proc->sibling);
    return proc;
}

void proc_get(struct proc *proc)
{
    refcount_inc(&proc->refcount);
}

void proc_put(struct proc *proc)
{
    if (!refcount_dec_and_test(&proc->refcount))
        return;
    Assert(!proc->tgid && !proc->nr_live && !proc->parent &&
               list_empty(&proc->members) && list_empty(&proc->children) &&
               list_empty(&proc->sibling),
           "cannot destroy a published proc without process teardown\n");
    kfree(proc);
}
