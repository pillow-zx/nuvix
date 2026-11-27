#ifndef _NUVIX_TASK_H
#define _NUVIX_TASK_H

#include <nuvix/atomic.h>
#include <nuvix/ctypes.h>
#include <nuvix/cleanup.h>
#include <nuvix/list.h>
#include <nuvix/macro.h>
#include <nuvix/refcount.h>
#include <nuvix/pid.h>
#include <nuvix/proc.h>
#include <nuvix/signal.h>
#include <nuvix/spinlock.h>
#include <nuvix/futex.h>
#include <asm/task.h>

struct fdtable;
struct fs_struct;
struct completion;

#define TASK_CPU_NONE        ((unsigned int)-1)

#define CLONE_VM             0x00000100UL
#define CLONE_FS             0x00000200UL
#define CLONE_FILES          0x00000400UL
#define CLONE_SIGHAND        0x00000800UL
#define CLONE_VFORK          0x00004000UL
#define CLONE_THREAD         0x00010000UL
#define CLONE_SETTLS         0x00080000UL
#define CLONE_PARENT_SETTID  0x00100000UL
#define CLONE_CHILD_CLEARTID 0x00200000UL

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
#define TASK_NAME_LEN        32

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

    struct pid *tid;
    struct proc *proc;
    struct hlist_node pid_links[PIDTYPE_MAX];
    struct list_head thread;
    char name[TASK_NAME_LEN];

    struct mm_struct *mm;
    struct fdtable *fdtable;
    struct completion *vfork_done;
    struct fs_struct *fs;

    int exit;

    struct robust_list_head *robust;
    uint32_t *clear_child_tid;

    spinlock_t signal_lock;
    struct sigpending pending;
    sigset_t blocked;
    sigset_t sigmask;
    stack_t sigstack;
    struct sighand *sighand;
};

extern struct task boot_task;
extern struct task *init;

struct task *task_alloc(void);

struct task *ktask_alloc(int (*fn)(void *), void *arg);

void task_free(struct task *task);

__noreturn void task_exit(int status);

__noreturn void task_exit_group(int status);

void task_check_group_exit(void);

void task_mm_release(void);

void task_vfork_done(struct task *task);

void task_finish_exit(struct task *task);

void task_get(struct task *task);

void task_put(struct task *task);

__noreturn void ktask_entry(struct task *prev, int (*fn)(void *), void *arg);

DECLARE_FREE(task_put, struct task *, if (_T) task_put(_T));
DECLARE_FREE(task, struct task *, if (_T) task_free(_T));

#define task_test_flag(task, mask)  (!!(atomic_read(&(task)->flags) & (mask)))
#define task_set_flag(task, mask)   atomic_or((mask), &(task)->flags)
#define task_clear_flag(task, mask) atomic_and(~(mask), &(task)->flags)

static inline bool task_group_exiting(const struct task *task)
{
    return task->proc && atomic_read_acquire(&task->proc->exiting);
}

// clang-format off
#define task_need_resched(t)        task_test_flag((t), TF_NEED_RESCHED)
#define task_set_need_resched(t, v)                                            \
    do {                                                                       \
        if (v) task_set_flag((t), TF_NEED_RESCHED);                            \
        else task_clear_flag((t), TF_NEED_RESCHED);                            \
    } while (0)

#define signal_pending(t)                                                      \
    ({                                                                         \
        const struct task *__pending_task = (t);                               \
        task_test_flag(__pending_task, TF_SIGPENDING) ||                        \
            task_group_exiting(__pending_task);                                \
    })
#define signal_pending_state(s, t)                                             \
    ({                                                                         \
        unsigned int __s = (s);                                                \
        const struct task *__t = (t);                                          \
        ((__s & TASK_INTERRUPTIBLE) && signal_pending(__t)) ||                 \
            ((__s & TASK_KILLABLE) &&                                          \
             ((READ_ONCE(__t->pending.signal) & sigmask(SIGKILL)) ||            \
              task_group_exiting(__t)));                                      \
    })
// clang-format on

static inline unsigned int current_cpu_id(void)
{
    return current->cpu_id;
}

#endif /* _NUVIX_TASK_H */
