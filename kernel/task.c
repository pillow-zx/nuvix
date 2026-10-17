#include <nuvix/task.h>
#include <nuvix/fdtable.h>
#include <nuvix/mm.h>
#include <nuvix/page.h>
#include <nuvix/slab.h>
#include <nuvix/pid.h>
#include <nuvix/layout.h>
#include <nuvix/assert.h>
#include <nuvix/preempt.h>
#include <nuvix/sched.h>

static_assert(KSTACK_ORDER < CONFIG_MAX_ORDER,
              "kernel task stack order must be smaller than MAX_ORDER");
static_assert(offsetof(struct task, kstack) == TASK_KSTACK,
              "invalid task stack offset");
static_assert(offsetof(struct task, ctx.ra) == TASK_CTX_RA,
              "invalid task context offset");
static_assert(offsetof(struct task, ctx.sum) == TASK_CTX_SUM,
              "invalid task SUM context offset");

struct task boot_task = {
    .refcount = REFCOUNT_INIT(1),
    .cpu_id = 0,
    .state = TASK_NEW,
    .on_cpu = true,
    .rq_node = LIST_INIT_HEAD(boot_task.rq_node),
    .children = LIST_INIT_HEAD(boot_task.children),
    .sibling = LIST_INIT_HEAD(boot_task.sibling),
    .preempt_count = INIT_PREEMPT_COUNT,
};

void task_boot_init(void *kstack)
{
    boot_task.kstack = kstack;
}

struct task *task_alloc(bool user)
{
    struct task *task __free(kfree) = kzalloc(sizeof(*task));

    if (!task)
        return NULL;
    struct page *page __free(kstack) = alloc_pages(KSTACK_ORDER);

    if (!page)
        return NULL;
    int tid __free(pid) = 0;
    struct mm_struct *mm __free(mm_put) = NULL;
    struct fdtable *files __free(kfree) = NULL;

    if (user) {
        tid = pid_alloc();
        if (tid < 0)
            return NULL;
        mm = mm_alloc();
        if (!mm)
            return NULL;
        files = kzalloc(sizeof(*files));
        if (!files)
            return NULL;
        if (fdtable_init(files))
            return NULL;
    }

    refcount_set(&task->refcount, 1);
    task->tid = (unsigned int)move(tid);
    task->tgid = task->tid;
    task->parent = user ? task : NULL;
    task->cpu_id = TASK_CPU_NONE;
    task->state = TASK_NEW;
    INIT_LIST_HEAD(&task->rq_node);
    INIT_LIST_HEAD(&task->children);
    INIT_LIST_HEAD(&task->sibling);
    task->preempt_count = INIT_PREEMPT_COUNT;
    task->kstack = (void *)__va(page_to_phys(move(page)));
    task->ctx.sp = (vaddr_t)task->kstack + KSTACK_SIZE;
    task->mm = move(mm);
    task->fdtable = move(files);
    return move(task);
}

void task_get(struct task *task)
{
    refcount_inc(&task->refcount);
}

void task_reap(struct task *task)
{
    might_sleep();
    Assert(task->state == TASK_DEAD && !task->on_cpu && !task->on_rq &&
               list_empty(&task->rq_node),
           "task reaping requires completed exit and switch teardown\n");
    free_pages(phys_to_page(__pa((vaddr_t)task->kstack)), KSTACK_ORDER);
    task->kstack = NULL;
    task_put(task);
}

void task_put(struct task *task)
{
    if (!refcount_dec_and_test(&task->refcount))
        return;
    Assert(task->state == TASK_NEW ||
               (task->state == TASK_DEAD && !task->kstack),
           "cannot destroy a published task without scheduler teardown\n");
    Assert((!task->parent || task->parent == task) &&
               list_empty(&task->children) && list_empty(&task->sibling),
           "cannot destroy a task linked in the process tree\n");
    if (task->state == TASK_NEW) {
        free_pages(phys_to_page(__pa((vaddr_t)task->kstack)), KSTACK_ORDER);
        task->kstack = NULL;
    }
    if (task->parent) {
        mm_put(task->mm);
        fdtable_destroy(task->fdtable);
        kfree(task->fdtable);
        pid_free(task->tid);
    }
    kfree(task);
}
