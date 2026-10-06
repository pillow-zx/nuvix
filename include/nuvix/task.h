#ifndef _NUVIX_TASK_H
#define _NUVIX_TASK_H

#include <nuvix/ctypes.h>
#include <nuvix/cleanup.h>
#include <nuvix/list.h>
#include <nuvix/refcount.h>
#include <asm/current.h>
#include <asm/task.h>

struct proc;

#define TASK_CPU_NONE ((unsigned int)-1)

enum task_state {
    TASK_NEW,
    TASK_RUNNABLE,
    TASK_SLEEP_INTERRUPTIBLE,
    TASK_SLEEP_UNINTERRUPTIBLE,
    TASK_STOPPED,
    TASK_DEAD,
};

struct task {
    refcount_t refcount;
    /* Fixed at construction; NULL for kernel tasks. */
    struct proc *proc;
    unsigned int tid;

    /* Owned by rq once published; CPU is assigned at first publication. */
    unsigned int cpu_id;
    enum task_state state;
    bool on_rq;
    bool on_cpu;
    /* Atomic hint; published writes and consumption stay under rq.lock. */
    bool need_resched;
    /* Ready-list membership, or dead-list membership after the final switch. */
    struct list_head rq_node;
    struct list_head proc_node;
    unsigned int preempt_count;

    /* Direct-mapped kernel stack base; all tasks use the configured order. */
    void *kstack;
    struct task_context ctx;

    /* Kernel thread entry and caller-owned argument. */
    int (*thread_fn)(void *);
    void *thread_arg;
};

/* Permanent boot context; architecture startup supplies the existing stack. */
extern struct task boot_task;
void task_boot_init(void *kstack);

/*
 * After slub_init(). Returns an unpublished NEW task with one caller reference,
 * a reserved TID and no CPU assignment. A leader requires a non-NULL proc and
 * uses its TGID; other tasks reserve an independent TID. The caller must create
 * at most one leader per proc. A non-NULL proc must be kept alive by the caller;
 * success takes an additional reference, without registering a member.
 * Allocation failure returns NULL and leaves the proc's references unchanged.
 */
struct task *task_alloc(struct proc *proc, bool leader);

/* Reaper-only, outside short locks: releases the stack and scheduler reference. */
void task_reap(struct task *task);

/* get requires a held reference or equivalent lifetime protection. */
void task_get(struct task *task);
/*
 * Final put supports unpublished NEW tasks and DEAD tasks whose stacks have
 * been reaped, in a context safe for allocator frees. Releases any remaining
 * NEW stack, independent TID and construction-time proc reference.
 */
void task_put(struct task *task);

DECLARE_FREE(task_put, struct task *, if (_T) task_put(_T));

static __always_inline bool task_need_resched(const struct task *task)
{
    return __atomic_load_n(&task->need_resched, __ATOMIC_RELAXED);
}

static __always_inline void task_set_need_resched(struct task *task, bool value)
{
    __atomic_store_n(&task->need_resched, value, __ATOMIC_RELAXED);
}

/* CPU-local use requires the execution context to remain on this CPU. */
static inline unsigned int current_cpu_id(void)
{
    return current->cpu_id;
}

#endif /* _NUVIX_TASK_H */
