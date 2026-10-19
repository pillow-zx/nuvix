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
    struct mm_struct *mm __free(mm_put) = NULL;
    struct fdtable *files __free(kfree) = NULL;

    if (user) {
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

static void free_resource(struct task *task)
{
    struct mm_struct *mm __free(mm_put) = move(task->mm);
    struct fdtable *fdt __free(kfree) = move(task->fdtable);
    fdtable_destroy(fdt);
}

void task_free(struct task *task)
{
    Assert(task->state == TASK_NEW, "cannot discard a published task\n");
    if (task->parent)
        free_resource(task);
    free_pages(phys_to_page(__pa((vaddr_t)task->kstack)), KSTACK_ORDER);
    task->kstack = NULL;
    task_put(task);
}

__noreturn void task_exit(void)
{
    struct task *task = current;
    might_sleep();
    if (task->parent) {
        Assert(task_pid_nr(task, PIDTYPE_TGID) != PID_INIT,
               "init cannot exit\n");
        pid_unpublish(task);
        free_resource(task);
    }
    preempt_disable();
    sched_exit();
}

void task_finish_exit(struct task *task)
{
    Assert(task->state == TASK_DEAD && !task->on_cpu && !task->on_rq &&
               list_empty(&task->rq_node) && !task->mm && !task->fdtable,
           "task exit requires completed resource cleanup and switch\n");
    free_pages(phys_to_page(__pa((vaddr_t)task->kstack)), KSTACK_ORDER);
    task->kstack = NULL;
    task_put(task);
}

void task_put(struct task *task)
{
    if (!refcount_dec_and_test(&task->refcount))
        return;
    Assert((task->state == TASK_NEW || task->state == TASK_DEAD) &&
               !task->on_cpu && !task->on_rq && list_empty(&task->rq_node) &&
               !task->kstack && !task->mm && !task->fdtable,
           "cannot destroy a task before resource and scheduler teardown\n");
    Assert((!task->parent || task->parent == task) &&
               list_empty(&task->children) && list_empty(&task->sibling),
           "cannot destroy a task linked in the process tree\n");
    for (enum pid_type type = 0; type < PIDTYPE_MAX; ++type) {
        Assert(hlist_unhashed(&task->pid_links[type]),
               "cannot destroy a task with published identities\n");
        struct pid *pid __free(pid_put) = move(task->pids[type]);
    }
    kfree(task);
}
