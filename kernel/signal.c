#include <nuvix/signal.h>
#include <nuvix/task.h>
#include <nuvix/spinlock.h>
#include <nuvix/slab.h>
#include <nuvix/errno.h>
#include <nuvix/cleanup.h>
#include <nuvix/pid.h>
#include <nuvix/string.h>
#include <nuvix/list.h>
#include <nuvix/macro.h>
#include <nuvix/math.h>
#include <nuvix/uaccess.h>
#include <nuvix/syscall.h>
#include <nuvix/sched.h>
#include <nuvix/bitmap.h>
#include <asm/signal.h>
#include <asm/trap.h>

// clang-format off
#define SIG_IGNORED     (sigmask(SIGCHLD) | sigmask(SIGCONT) | \
                        sigmask(SIGURG) | sigmask(SIGWINCH))
// clang-format on

struct sighand *sighand_alloc(void)
{
    struct sighand *sighand = kzalloc(sizeof(*sighand));

    if (!sighand)
        return NULL;
    spin_lock_init(&sighand->lock);
    refcount_set(&sighand->refs, 1);
    return sighand;
}

void sighand_put(struct sighand *sighand)
{
    if (sighand && refcount_dec_and_test(&sighand->refs))
        kfree(sighand);
}

static inline void sighand_clone(struct sighand *dst, struct sighand *src)
{
    spin_lock(&src->lock);
    memcpy(dst->actions, src->actions, sizeof(dst->actions));
    spin_unlock(&src->lock);
}

static inline bool signal_ignored(const struct sigaction *action, int sig)
{
    return action->sa_handler == SIG_IGN ||
           (action->sa_handler == SIG_DFL && (SIG_IGNORED & sigmask(sig)));
}

static inline void signal_default(int sig)
{
    if (SIG_IGNORED & sigmask(sig))
        return;

    switch (sig) {
    case SIGSTOP:
    case SIGTSTP:
    case SIGTTIN:
    case SIGTTOU:
        sched_stop();
        return;
    default:
        task_exit_group(sig);
    }
}

void recalc_sigpending(struct task *task)
{
    assert_spin_locked(&task->signal_lock);
    if (task->pending.signal & ~task->blocked)
        task_set_flag(task, TF_SIGPENDING);
    else
        task_clear_flag(task, TF_SIGPENDING);
}

void signal_task_init(struct task *task)
{
    spin_lock_init(&task->signal_lock);
    INIT_LIST_HEAD(&task->pending.list);
    task->pending.signal = 0;
    task->blocked = 0;
    task->sigstack = (stack_t){.ss_flags = SS_DISABLE};
}

int signal_task_clone(struct task *child, bool share)
{
    struct sighand *sighand = current->sighand;

    if (share) {
        refcount_inc(&sighand->refs);
        child->sighand = sighand;
    } else {
        child->sighand = sighand_alloc();
        if (!child->sighand)
            return -ENOMEM;
        sighand_clone(child->sighand, sighand);
    }
    return 0;
}

void signal_task_free(struct task *task)
{
    struct sigqueue *queue, *next;

    list_for_each_entry_safe (queue, next, &task->pending.list, list) {
        struct sigqueue *entry __free(kfree) = queue;

        list_del_init(&entry->list);
    }
    task->pending.signal = 0;
    sighand_put(move(task->sighand));
}

static void flush_signals_locked(struct task *task, sigset_t mask)
{
    struct sigqueue *queue, *next;

    assert_spin_locked(&task->signal_lock);
    list_for_each_entry_safe (queue, next, &task->pending.list, list) {
        if (!(mask & sigmask(queue->info.si_signo)))
            continue;
        list_del_init(&queue->list);
        kfree(queue);
    }
    task->pending.signal &= ~mask;
}

int send_signal(struct task *task, const siginfo_t *info)
{
    struct sigqueue *queue __free(kfree) = NULL;
    struct sighand *sighand;
    int sig = info->si_signo;
    bool notify;

    if (!sig)
        return 0;

    queue = kmalloc(sizeof(*queue));
    if (queue)
        queue->info = *info;

    spin_lock(&task->signal_lock);
    if (SIG_STOPMASK & sigmask(sig))
        flush_signals_locked(task, sigmask(SIGCONT));
    else if (sig == SIGCONT)
        flush_signals_locked(task, SIG_STOPMASK);
    if (sig == SIGCONT || sig == SIGKILL)
        task_clear_flag(task, TF_STOPPENDING);
    sighand = task->sighand;
    spin_lock(&sighand->lock);
    if (sig_valid(task, sig) &&
        ((task->blocked & sigmask(sig)) ||
         !signal_ignored(&sighand->actions[sig - 1], sig))) {
        if (queue) {
            list_add_tail(&queue->list, &task->pending.list);
            keep(queue);
        }
        task->pending.signal |= sigmask(sig);
    }
    spin_unlock(&sighand->lock);
    recalc_sigpending(task);
    if (sig == SIGCONT || sig == SIGKILL)
        sched_resume(task);
    notify = signal_pending(task);
    spin_unlock(&task->signal_lock);

    if (notify)
        sched_wake_signal(task, sig);
    return 0;
}

static void dequeue_signal_locked(struct task *task, int sig, siginfo_t *info)
{
    struct sigqueue *queue;

    assert_spin_locked(&task->signal_lock);
    task->pending.signal &= ~sigmask(sig);
    list_for_each_entry (queue, &task->pending.list, list) {
        if (queue->info.si_signo != sig)
            continue;
        *info = queue->info;
        list_del_init(&queue->list);
        kfree(queue);
        return;
    }
    *info = (siginfo_t){.si_signo = sig, .si_code = SI_USER};
}

static void wait_for_signal(void)
{
    struct task *task = current;
    struct rq *rq = this_rq();
    unsigned long flags;

    might_sleep();
    for (;;) {
        preempt_disable();
        spin_lock_irqsave(&rq->lock, flags);
        if (signal_pending(task)) {
            spin_unlock_irqrestore(&rq->lock, flags);
            preempt_enable();
            return;
        }
        sched_set_sleep_locked(rq, TASK_INTERRUPTIBLE);
        spin_unlock_irqrestore(&rq->lock, flags);
        sched_park();
    }
}

static int pick_signal_locked(struct task *task, siginfo_t *info,
                              const siginfo_t *fault)
{
    sigset_t pending;
    int sig;

    if (fault) {
        sig = fault->si_signo;
        *info = *fault;
        if (sig_pending(task, sig))
            dequeue_signal_locked(task, sig, info);
        return sig;
    }

    pending = task->pending.signal & ~task->blocked;
    if (!pending)
        return 0;
    sig = (pending & sigmask(SIGKILL)) ? SIGKILL
                                       : find_first_bit(&pending, SIG_MAX) + 1;
    dequeue_signal_locked(task, sig, info);
    return sig;
}

static bool prepare_signal_locked(struct task *task, struct ksignal *signal,
                                  int sig, bool fault)
{
    struct sighand *sighand = task->sighand;
    sighandler_t handler;
    bool fatal;

    spin_lock(&sighand->lock);
    signal->action = sighand->actions[sig - 1];
    handler = signal->action.sa_handler;
    signal->oldmask = task_test_flag(task, TF_SIGSUSPEND | TF_RESIGMASK)
                          ? task->sigmask : task->blocked;
    fatal = fault && ((task->blocked & sigmask(sig)) || handler == SIG_DFL ||
                      handler == SIG_IGN);
    if (!fatal && handler != SIG_DFL && handler != SIG_IGN &&
        (signal->action.sa_flags & SA_RESETHAND))
        sighand->actions[sig - 1].sa_handler = SIG_DFL;
    spin_unlock(&sighand->lock);

    if (handler == SIG_DFL && (SIG_STOPMASK & sigmask(sig)))
        task_set_flag(task, TF_STOPPENDING);
    recalc_sigpending(task);
    return fatal;
}

bool get_signal(struct ksignal *signal, const siginfo_t *fault)
{
    struct task *task = current;

    for (;;) {
        int sig;
        bool fatal;

        task_check_group_exit();
        spin_lock(&task->signal_lock);
        sig = pick_signal_locked(task, &signal->info, fault);
        if (!sig) {
            if (task_test_flag(task, TF_RESIGMASK)) {
                task->blocked = task->sigmask;
                task_clear_flag(task, TF_RESIGMASK);
                recalc_sigpending(task);
                spin_unlock(&task->signal_lock);
                continue;
            }
            spin_unlock(&task->signal_lock);
            if (task_test_flag(task, TF_SIGSUSPEND)) {
                wait_for_signal();
                continue;
            }
            return false;
        }

        fatal = prepare_signal_locked(task, signal, sig, fault != NULL);
        spin_unlock(&task->signal_lock);

        if (fatal)
            task_exit_group(sig);
        if (signal->action.sa_handler == SIG_IGN)
            continue;
        if (signal->action.sa_handler == SIG_DFL) {
            signal_default(sig);
            continue;
        }
        return true;
    }
}

void signal_delivered(const struct ksignal *signal)
{
    struct task *task = current;

    spin_lock(&task->signal_lock);
    task_clear_flag(task, TF_SIGSUSPEND | TF_RESIGMASK);
    task->blocked |= signal->action.sa_mask;
    if (!(signal->action.sa_flags & SA_NODEFER))
        task->blocked |= sigmask(signal->info.si_signo);
    recalc_sigpending(task);
    spin_unlock(&task->signal_lock);
}

long sys_kill(struct trap_frame *frame)
{
    pid_t pid = syscallarg(frame, 0);
    int sig = syscallarg(frame, 1);
    struct task *task __free(task_put) = NULL;
    siginfo_t info;

    if (!IN_RANGE(sig, 0, SIG_MAX))
        return -EINVAL;

    memset(&info, 0, sizeof(info));
    info.si_signo = sig;
    info.si_code = SI_USER;
    info.si_pid = task_pid_nr(current, PIDTYPE_TGID);
    info.si_uid = 0;

    if (pid == 0)
        return send_group_signal(0, &info);

    if (pid < -1) {
        if (pid < -PID_MAX)
            return -ESRCH;
        return send_group_signal(ABS(pid), &info);
    }

    if (pid == -1)
        return broadcast_signal(&info);

    task = pid_lookup_task(pid, PIDTYPE_TGID);
    if (!task)
        return -ESRCH;

    return send_signal(task, &info);
}

long sys_tkill(struct trap_frame *frame)
{
    pid_t tid = syscallarg(frame, 0);
    int sig = syscallarg(frame, 1);
    struct task *task __free(task_put) = NULL;
    siginfo_t info;

    if (tid <= 0 || !IN_RANGE(sig, 0, SIG_MAX))
        return -EINVAL;

    task = pid_lookup_task(tid, PIDTYPE_TID);
    if (!task)
        return -ESRCH;

    memset(&info, 0, sizeof(info));
    info.si_signo = sig;
    info.si_code = SI_TKILL;
    info.si_pid = task_pid_nr(current, PIDTYPE_TGID);
    info.si_uid = 0;

    return send_signal(task, &info);
}

long sys_tgkill(struct trap_frame *frame)
{
    pid_t tgid = syscallarg(frame, 0);
    pid_t tid = syscallarg(frame, 1);
    int sig = syscallarg(frame, 2);
    struct task *task __free(task_put) = NULL;
    siginfo_t info;

    if (tgid <= 0 || tid <= 0 || !IN_RANGE(sig, 0, SIG_MAX))
        return -EINVAL;

    task = pid_lookup_task(tid, PIDTYPE_TID);
    if (!task)
        return -ESRCH;

    if (task_pid_nr(task, PIDTYPE_TGID) != tgid)
        return -ESRCH;

    memset(&info, 0, sizeof(info));
    info.si_signo = sig;
    info.si_code = SI_TKILL;
    info.si_pid = task_pid_nr(current, PIDTYPE_TGID);
    info.si_uid = 0;

    return send_signal(task, &info);
}

long sys_rt_sigsuspend(struct trap_frame *frame)
{
    const sigset_t *mask = (const sigset_t *)syscallarg(frame, 0);
    size_t sigsetsize = syscallarg(frame, 1);
    struct task *task = current;
    sigset_t set;

    if (sigsetsize != SIGSET_SIZE)
        return -EINVAL;

    if (copy_from_user(&set, mask, sizeof(set)))
        return -EFAULT;

    spin_lock(&task->signal_lock);
    task->sigmask = task->blocked;
    task->blocked = set & ~SIG_UNBLOCKMASK;
    task_set_flag(task, TF_SIGSUSPEND);
    recalc_sigpending(task);
    spin_unlock(&task->signal_lock);

    wait_for_signal();
    return -EINTR;
}

long sys_rt_sigaction(struct trap_frame *frame)
{
    int sig = syscallarg(frame, 0);
    const struct sigaction *act =
        (const struct sigaction *)syscallarg(frame, 1);
    struct sigaction *oldact = (struct sigaction *)syscallarg(frame, 2);
    size_t sigsetsize = syscallarg(frame, 3);

    struct task *task = current;
    struct sigaction new, old;
    struct sighand *sighand;

    if (sigsetsize != SIGSET_SIZE || sig == 0 || !IN_RANGE(sig, 0, SIG_MAX))
        return -EINVAL;

    if (act) {
        if (sig == SIGKILL || sig == SIGSTOP)
            return -EINVAL;
        if (copy_from_user(&new, act, sizeof(new)))
            return -EFAULT;
        new.sa_mask &= ~SIG_UNBLOCKMASK;
    }

    spin_lock(&task->signal_lock);
    sighand = task->sighand;
    spin_lock(&sighand->lock);
    old = sighand->actions[sig - 1];
    if (act) {
        sighand->actions[sig - 1] = new;
        if (signal_ignored(&new, sig))
            flush_signals_locked(task, sigmask(sig));
        recalc_sigpending(task);
    }
    spin_unlock(&sighand->lock);
    spin_unlock(&task->signal_lock);

    if (oldact) {
        if (copy_to_user(oldact, &old, sizeof(old)))
            return -EFAULT;
    }

    return 0;
}

long sys_rt_sigprocmask(struct trap_frame *frame)
{
    int how = syscallarg(frame, 0);
    const sigset_t *set = (const sigset_t *)syscallarg(frame, 1);
    sigset_t *oldset = (sigset_t *)syscallarg(frame, 2);
    size_t sigsetsize = syscallarg(frame, 3);
    struct task *task = current;
    sigset_t old, new;

    if (sigsetsize != SIGSET_SIZE)
        return -EINVAL;

    if (set) {
        if (copy_from_user(&new, set, sizeof(new)))
            return -EFAULT;
        new &= ~SIG_UNBLOCKMASK;
    }

    spin_lock(&task->signal_lock);
    old = task->blocked;

    if (set) {
        switch (how) {
        case SIG_BLOCK:
            task->blocked |= new;
            break;
        case SIG_UNBLOCK:
            task->blocked &= ~new;
            break;
        case SIG_SETMASK:
            task->blocked = new;
            break;
        default:
            spin_unlock(&task->signal_lock);
            return -EINVAL;
        }
        recalc_sigpending(task);
    }

    spin_unlock(&task->signal_lock);

    if (oldset && copy_to_user(oldset, &old, sizeof(old)))
        return -EFAULT;

    return 0;
}

long sys_rt_sigpending(struct trap_frame *frame)
{
    sigset_t *set = (sigset_t *)syscallarg(frame, 0);
    const size_t sigsetsize = (const size_t)syscallarg(frame, 1);
    struct task *task = current;
    sigset_t ret;

    if (sigsetsize > sizeof(ret))
        return -EINVAL;

    spin_lock(&task->signal_lock);
    ret = task->pending.signal & task->blocked;
    spin_unlock(&task->signal_lock);

    if (copy_to_user(set, &ret, sigsetsize))
        return -EFAULT;

    return 0;
}

int sigstack_flags(unsigned long sp)
{
    const stack_t *stack = &current->sigstack;
    unsigned long base = (unsigned long)stack->ss_sp;

    if (!stack->ss_size)
        return SS_DISABLE;
    return sp > base && sp - base <= stack->ss_size ? SS_ONSTACK : 0;
}

int signal_set_stack(const stack_t *stack, unsigned long sp)
{
    if (sigstack_flags(sp) == SS_ONSTACK)
        return -EPERM;
    if (stack->ss_flags != 0 && stack->ss_flags != SS_DISABLE &&
        stack->ss_flags != SS_ONSTACK)
        return -EINVAL;
    if (stack->ss_flags == SS_DISABLE) {
        current->sigstack = (stack_t){.ss_flags = SS_DISABLE};
        return 0;
    }
    if (stack->ss_size < MINSIGSTKSZ)
        return -ENOMEM;
    current->sigstack = (stack_t){
        .ss_sp = stack->ss_sp,
        .ss_size = stack->ss_size,
    };
    return 0;
}

long sys_sigaltstack(struct trap_frame *frame)
{
    const stack_t *uss = (const stack_t *)syscallarg(frame, 0);
    stack_t *uoss = (stack_t *)syscallarg(frame, 1);
    unsigned long sp = frame->regs[2];
    stack_t new, old = {0};
    int ret;

    if (uss && copy_from_user(&new, uss, sizeof(new)))
        return -EFAULT;
    old.ss_sp = current->sigstack.ss_sp;
    old.ss_size = current->sigstack.ss_size;
    old.ss_flags = sigstack_flags(sp);
    if (uss) {
        ret = signal_set_stack(&new, sp);
        if (ret)
            return ret;
    }
    if (uoss && copy_to_user(uoss, &old, sizeof(old)))
        return -EFAULT;
    return 0;
}

void signal_exec(struct sighand *new)
{
    struct task *task = current;
    struct sighand *old = NULL;

    spin_lock(&task->signal_lock);
    if (new) {
        old = task->sighand;
        sighand_clone(new, old);
        task->sighand = new;
    }
    task->sigstack = (stack_t){.ss_flags = SS_DISABLE};
    for (unsigned int i = 0; i < SIG_MAX; ++i) {
        struct sigaction *action = &task->sighand->actions[i];

        if (action->sa_handler != SIG_IGN)
            action->sa_handler = SIG_DFL;
        action->sa_flags = 0;
        action->sa_mask = 0;
    }
    spin_unlock(&task->signal_lock);
    sighand_put(old);
}
