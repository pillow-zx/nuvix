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
#include <nuvix/path.h>
#include <nuvix/futex.h>
#include <asm/task.h>

struct fdtable;
struct completion;

#define TASK_CPU_NONE        ((unsigned int)-1)

#define TF_NEED_RESCHED      (1U << 0)
#define TF_FORKNOEXEC        (1U << 1)
#define TF_SIGPENDING        (1U << 2)
#define TF_STOPPENDING       (1U << 3)
#define TF_SIGSUSPEND        (1U << 4)
#define TF_RESIGMASK         (1U << 5)

#define TASK_NEW             0U
#define TASK_RUNNABLE        (1U << 0)
#define TASK_INTERRUPTIBLE   (1U << 1)
#define TASK_UNINTERRUPTIBLE (1U << 2)
#define TASK_STOPPED         (1U << 3)
#define TASK_ZOMBIE          (1U << 4)
#define TASK_KILLABLE        (1U << 5)

#define TASK_NORMAL          (TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)

struct task {
    refcount_t refcount;

    /* Owned by rq once published; CPU is assigned at first publication. */
    unsigned int cpu_id;
    unsigned int state;
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
    struct completion *vfork_done;
    struct path cwd;

    /*
     * NULL for kernel tasks; user tasks self-parent until attached to a parent,
     * and init remains self-parented. Detaching restores self-parenting.
     */
    struct task *parent;
    struct list_head children;
    struct list_head sibling;
    int exit;

    struct robust_list_head *robust;
    uint32_t *clear_child_tid;

    spinlock_t signal_lock;
    struct sigpending pending;
    sigset_t blocked;
    sigset_t sigmask;
    stack_t sigstack;
    struct sigaction actions[SIG_MAX];
};

extern struct task boot_task;
extern struct task *init;

struct task *task_alloc(bool user);

void task_free(struct task *task);

__noreturn void task_exit(int status);

void task_mm_release(void);

void task_vfork_done(struct task *task);

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

static __always_inline bool signal_pending(const struct task *task)
{
    return task_test_flag(task, TF_SIGPENDING);
}

static inline bool signal_pending_state(unsigned int state,
                                        const struct task *task)
{
    return ((state & TASK_INTERRUPTIBLE) && signal_pending(task)) ||
           ((state & TASK_KILLABLE) &&
            (READ_ONCE(task->pending.signal) & sigmask(SIGKILL)));
}

static inline unsigned int current_cpu_id(void)
{
    return current->cpu_id;
}

#endif /* _NUVIX_TASK_H */
