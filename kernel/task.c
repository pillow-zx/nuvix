#include <nuvix/task.h>
#include <nuvix/proc.h>
#include <nuvix/page.h>
#include <nuvix/slab.h>
#include <nuvix/pid.h>
#include <nuvix/layout.h>
#include <nuvix/assert.h>
#include <nuvix/preempt.h>

static_assert(KSTACK_ORDER < CONFIG_MAX_ORDER,
              "kernel task stack order must be smaller than MAX_ORDER");

struct task boot_task = {
    .refcount = REFCOUNT_INIT(1),
    .cpu_id = 0,
    .state = TASK_NEW,
    .on_cpu = true,
    .rq_node = LIST_INIT_HEAD(boot_task.rq_node),
    .proc_node = LIST_INIT_HEAD(boot_task.proc_node),
    .preempt_count = INIT_PREEMPT_COUNT,
};

void task_boot_init(void *kstack)
{
    boot_task.kstack = kstack;
}

struct task *task_alloc(struct proc *proc, bool leader)
{
    struct task *task __free(kfree) = kzalloc(sizeof(*task));
    struct page *page __free(kstack) = NULL;
    int tid __free(pid) = 0;

    Assert(!leader || proc, "task leader requires a proc\n");
    if (!task)
        return NULL;
    page = alloc_pages(KSTACK_ORDER);
    if (!page)
        return NULL;
    if (!leader) {
        tid = pid_alloc();
        if (tid < 0)
            return NULL;
    }

    refcount_set(&task->refcount, 1);
    task->tid = leader ? proc->tgid : move(tid);
    task->cpu_id = TASK_CPU_NONE;
    task->state = TASK_NEW;
    INIT_LIST_HEAD(&task->rq_node);
    INIT_LIST_HEAD(&task->proc_node);
    task->kstack = (void *)__va(page_to_phys(move(page)));
    task->ctx.sp = (vaddr_t)task->kstack + KSTACK_SIZE;
    if (proc)
        proc_get(proc);
    task->proc = proc;
    return move(task);
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
    if (!task->proc || task->tid != task->proc->tgid)
        pid_free(task->tid);
    if (task->proc)
        proc_put(task->proc);
    kfree(task);
}
