#ifndef _NUVIX_TASK_H
#define _NUVIX_TASK_H

#include <nuvix/atomic.h>
#include <nuvix/ctypes.h>
#include <nuvix/cleanup.h>
#include <nuvix/list.h>
#include <nuvix/refcount.h>
#include <nuvix/pid.h>
#include <nuvix/signal.h>
#include <nuvix/spinlock.h>
#include <asm/task.h>

struct fdtable;

#define TASK_CPU_NONE   ((unsigned int)-1)

#define TF_NEED_RESCHED (1U << 0)
#define TF_EXEC         (1U << 1)

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

    /* Owned by rq once published; CPU is assigned at first publication. */
    unsigned int cpu_id;
    enum task_state state;
    bool on_rq;
    bool on_cpu;

    struct list_head rq_node;
    unsigned int preempt_count;

    atomic_t flags;

    void *kstack;
    struct task_context ctx;

    struct pid *pids[PIDTYPE_MAX];
    struct hlist_node pid_links[PIDTYPE_MAX];

    struct mm_struct *mm;
    struct fdtable *fdtable;

    /*
     * NULL for kernel tasks; user tasks self-parent until attached to a parent,
     * and init remains self-parented. Detaching restores self-parenting.
     * Parent/child links will be protected by the process-tree lock.
     */
    struct task *parent;
    struct list_head children;
    struct list_head sibling;

    spinlock_t signal_lock;
    struct sigpending pending;
    sigset_t blocked;
    struct sigaction actions[SIG_MAX];
};

extern struct task boot_task;

void task_boot_init(void *kstack);

struct task *task_alloc(bool user);

void task_free(struct task *task);

__noreturn void task_exit(void);

void task_finish_exit(struct task *task);

void task_get(struct task *task);

void task_put(struct task *task);

/* ktask, do not delete */
int ktask_init(void);
__noreturn void ktask_entry(struct task *prev, int (*fn)(void *), void *arg);
__noreturn void ktask_exit(void);

DECLARE_FREE(task_put, struct task *, if (_T) task_put(_T));
DECLARE_FREE(task, struct task *, if (_T) task_free(_T));

#define task_test_flag(task, mask)  (!!(atomic_read(&(task)->flags) & (mask)))
#define task_set_flag(task, mask)   atomic_or((mask), &(task)->flags)
#define task_clear_flag(task, mask) atomic_and(~(mask), &(task)->flags)

static __always_inline bool task_need_resched(const struct task *task)
{
    return task_test_flag(task, TF_NEED_RESCHED);
}

static __always_inline void task_set_need_resched(struct task *task, bool value)
{
    if (value)
        task_set_flag(task, TF_NEED_RESCHED);
    else
        task_clear_flag(task, TF_NEED_RESCHED);
}

static inline unsigned int current_cpu_id(void)
{
    return current->cpu_id;
}

#endif /* _NUVIX_TASK_H */
