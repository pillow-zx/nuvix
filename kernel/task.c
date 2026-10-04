#include <nuvix/task.h>
#include <nuvix/proc.h>
#include <nuvix/page.h>
#include <nuvix/slab.h>
#include <nuvix/layout.h>
#include <nuvix/assert.h>

static_assert(KSTACK_ORDER < CONFIG_MAX_ORDER,
              "kernel task stack order must be smaller than MAX_ORDER");

struct task *task_alloc(struct proc *proc)
{
    struct task *task = kzalloc(sizeof(*task));
    struct page *page;

    if (!task)
        return NULL;
    page = alloc_pages(KSTACK_ORDER);
    if (!page) {
        kfree(task);
        return NULL;
    }

    refcount_set(&task->refcount, 1);
    task->cpu_id = TASK_CPU_NONE;
    task->state = TASK_NEW;
    INIT_LIST_HEAD(&task->rq_node);
    INIT_LIST_HEAD(&task->proc_node);
    task->kstack = (void *)__va(page_to_phys(page));
    task->ctx.sp = (vaddr_t)task->kstack + KSTACK_SIZE;
    if (proc)
        proc_get(proc);
    task->proc = proc;
    return task;
}

void task_get(struct task *task)
{
    refcount_inc(&task->refcount);
}

void task_put(struct task *task)
{
    if (!refcount_dec_and_test(&task->refcount))
        return;
    Assert(task->state == TASK_NEW,
           "cannot destroy a published task without scheduler teardown\n");
    free_pages(phys_to_page(__pa((vaddr_t)task->kstack)), KSTACK_ORDER);
    if (task->proc)
        proc_put(task->proc);
    kfree(task);
}
