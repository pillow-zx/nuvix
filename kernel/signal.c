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

void sighand_get(struct sighand *sighand)
{
    refcount_inc(&sighand->refs);
}

void sighand_put(struct sighand *sighand)
{
    if (sighand && refcount_dec_and_test(&sighand->refs))
        kfree(sighand);
}

static void sighand_copy(struct sighand *dst, struct sighand *src)
{
    spin_lock(&src->lock);
    memcpy(dst->actions, src->actions, sizeof(dst->actions));
    spin_unlock(&src->lock);
}

struct sighand *sighand_clone(struct sighand *src)
{
    struct sighand *dst = sighand_alloc();

    if (dst)
        sighand_copy(dst, src);
    return dst;
}

static inline bool signal_ignored(const struct sigaction *action, int sig)
{
    return action->sa_handler == SIG_IGN ||
           (action->sa_handler == SIG_DFL && (SIG_IGNORED & sigmask(sig)));
}

static void update_sigpending(struct task *task)
{
    sigset_t pending = task->pending.signal | task->proc->pending.signal;

    if (!task_test_flag(task, TF_EXITING) && (pending & ~task->blocked))
        task_set_flag(task, TF_SIGPENDING);
    else
        task_clear_flag(task, TF_SIGPENDING);
}

static void wake_proc_signals(struct proc *proc)
{
    struct task *task;
    sigset_t pending = proc->pending.signal;

    if (!pending || atomic_read(&proc->exiting))
        return;
    for_each_thread (proc, task) {
        sigset_t ready = pending & ~task->blocked;

        if (!ready || task_test_flag(task, TF_EXITING) ||
            !(READ_ONCE(task->state) & (TASK_RUNNABLE | TASK_INTERRUPTIBLE)))
            continue;
        task_set_flag(task, TF_SIGPENDING);
        if (ready & SIG_STOPMASK)
            sched_wake_stop(task);
        else
            sched_wake_interruptible(task);
        pending &= ~ready;
        if (!pending)
            break;
    }
}

void recalc_sigpending(struct task *task)
{
    assert_spin_locked(&task->proc->lock);
    update_sigpending(task);
    wake_proc_signals(task->proc);
}

void signal_task_init(struct task *task)
{
    INIT_LIST_HEAD(&task->pending.list);
    task->pending.signal = 0;
    task->blocked = 0;
    task->sigstack = (stack_t){.ss_flags = SS_DISABLE};
}

void signal_task_free(struct task *task)
{
    flush_sigpending(&task->pending, ~(sigset_t)0);
    sighand_put(move(task->sighand));
}

void flush_sigpending(struct sigpending *pending, sigset_t mask)
{
    struct sigqueue *queue, *next;

    list_for_each_entry_safe (queue, next, &pending->list, list) {
        if (!(mask & sigmask(queue->info.si_signo)))
            continue;
        list_del_init(&queue->list);
        kfree(queue);
    }
    WRITE_ONCE(pending->signal, pending->signal & ~mask);
}

static void flush_proc_sigpending(struct proc *proc, sigset_t mask)
{
    struct task *thread;

    flush_sigpending(&proc->pending, mask);
    for_each_thread (proc, thread) {
        flush_sigpending(&thread->pending, mask);
        update_sigpending(thread);
    }
}

static void handle_jobctl(struct proc *proc, int sig)
{
    struct task *thread;

    if (SIG_STOPMASK & sigmask(sig)) {
        flush_proc_sigpending(proc, sigmask(SIGCONT));
    } else if (sig == SIGCONT) {
        flush_sigpending(&proc->pending, SIG_STOPMASK);
        for_each_thread (proc, thread) {
            flush_sigpending(&thread->pending, SIG_STOPMASK);
            update_sigpending(thread);
            task_clear_flag(thread, TF_STOPPENDING);
            sched_resume(thread);
        }
    }
}

static void queue_signal(struct proc *proc, struct task *task, int sig,
                         struct sigqueue *entry)
{
    struct sigqueue *queue __free(kfree) = entry;
    struct sigpending *pending = task ? &task->pending : &proc->pending;
    struct sighand *sighand = task ? task->sighand : proc->leader->sighand;
    sigset_t mask = sigmask(sig);
    struct task *thread;
    bool blocked = false;

    if (pending->signal & mask)
        return;
    if (task) {
        blocked = task->blocked & mask;
    } else {
        for_each_thread (proc, thread) {
            if (thread->blocked & mask) {
                blocked = true;
                break;
            }
        }
    }
    spin_lock(&sighand->lock);
    if (blocked || !signal_ignored(&sighand->actions[sig - 1], sig)) {
        if (queue) {
            list_add_tail(&queue->list, &pending->list);
            keep(queue);
        }
        WRITE_ONCE(pending->signal, pending->signal | mask);
    }
    spin_unlock(&sighand->lock);
}

int send_signal(struct proc *proc, struct task *task, const siginfo_t *info)
{
    struct sigqueue *queue __free(kfree) = NULL;
    int sig = info->si_signo;

    if (!sig)
        return 0;
    if (!task && sig == SIGKILL) {
        proc_kill(proc);
        return 0;
    }
    queue = kmalloc(sizeof(*queue));
    if (queue)
        queue->info = *info;

    spin_lock(&proc->lock);
    if (atomic_read(&proc->exiting) || list_empty(&proc->threads) ||
        (task && task_test_flag(task, TF_EXITING)))
        goto out;
    handle_jobctl(proc, sig);
    queue_signal(proc, task, sig, move(queue));
    if (task) {
        update_sigpending(task);
        if (signal_pending(task))
            sched_wake_signal(task, sig);
    }
    wake_proc_signals(proc);
out:
    spin_unlock(&proc->lock);
    return 0;
}

static void dequeue_signal_locked(struct sigpending *pending, int sig,
                                  siginfo_t *info)
{
    struct sigqueue *queue;

    WRITE_ONCE(pending->signal, pending->signal & ~sigmask(sig));
    list_for_each_entry (queue, &pending->list, list) {
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
    struct sigpending *queue = &task->pending;
    sigset_t pending;
    int sig;

    if (fault) {
        sig = fault->si_signo;
        *info = *fault;
        if (sig_pending(task, sig))
            dequeue_signal_locked(queue, sig, info);
        return sig;
    }

    pending = queue->signal & ~task->blocked;
    if (!pending) {
        queue = &task->proc->pending;
        pending = queue->signal & ~task->blocked;
    }
    if (!pending)
        return 0;
    sig = (pending & sigmask(SIGKILL)) ? SIGKILL
                                       : find_first_bit(&pending, SIG_MAX) + 1;
    dequeue_signal_locked(queue, sig, info);
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
                          ? task->sigmask
                          : task->blocked;
    fatal = fault && ((task->blocked & sigmask(sig)) || handler == SIG_DFL ||
                      handler == SIG_IGN);
    if (!fatal && handler != SIG_DFL && handler != SIG_IGN &&
        (signal->action.sa_flags & SA_RESETHAND))
        sighand->actions[sig - 1].sa_handler = SIG_DFL;
    spin_unlock(&sighand->lock);

    update_sigpending(task);
    return fatal;
}

bool get_signal(struct ksignal *signal, const siginfo_t *fault)
{
    struct task *task = current;

    might_sleep();
    for (;;) {
        int sig;
        bool fatal;

        spin_lock(&task->proc->lock);
        if (fatal_signal_pending(task)) {
            spin_unlock(&task->proc->lock);
            task_exit_group(SIGKILL);
        }
        if (task_test_flag(task, TF_STOPPENDING)) {
            spin_unlock(&task->proc->lock);
            sched_stop();
            continue;
        }
        sig = pick_signal_locked(task, &signal->info, fault);
        if (!sig) {
            if (task_test_flag(task, TF_RESIGMASK)) {
                WRITE_ONCE(task->blocked, task->sigmask);
                task_clear_flag(task, TF_RESIGMASK);
                recalc_sigpending(task);
                spin_unlock(&task->proc->lock);
                continue;
            }
            update_sigpending(task);
            spin_unlock(&task->proc->lock);
            if (task_test_flag(task, TF_SIGSUSPEND)) {
                wait_for_signal();
                continue;
            }
            return false;
        }

        fatal = prepare_signal_locked(task, signal, sig, fault != NULL);
        if (!fatal && signal->action.sa_handler == SIG_DFL &&
            (SIG_STOPMASK & sigmask(sig))) {
            task_set_flag(task, TF_STOPPENDING);
            spin_unlock(&task->proc->lock);
            continue;
        }
        spin_unlock(&task->proc->lock);

        if (fatal)
            task_exit_group(sig);
        if (signal->action.sa_handler == SIG_IGN)
            continue;
        if (signal->action.sa_handler == SIG_DFL) {
            if (SIG_IGNORED & sigmask(sig))
                continue;
            task_exit_group(sig);
        }
        return true;
    }
}

void signal_delivered(const struct ksignal *signal)
{
    struct task *task = current;
    sigset_t blocked;

    spin_lock(&task->proc->lock);
    task_clear_flag(task, TF_SIGSUSPEND | TF_RESIGMASK);
    blocked = task->blocked | signal->action.sa_mask;
    if (!(signal->action.sa_flags & SA_NODEFER))
        blocked |= sigmask(signal->info.si_signo);
    WRITE_ONCE(task->blocked, blocked);
    recalc_sigpending(task);
    spin_unlock(&task->proc->lock);
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

    return send_signal(task->proc, NULL, &info);
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

    return send_signal(task->proc, task, &info);
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

    return send_signal(task->proc, task, &info);
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

    spin_lock(&task->proc->lock);
    task->sigmask = task->blocked;
    WRITE_ONCE(task->blocked, set & ~SIG_UNBLOCKMASK);
    task_set_flag(task, TF_SIGSUSPEND);
    recalc_sigpending(task);
    spin_unlock(&task->proc->lock);

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

    spin_lock(&task->proc->lock);
    sighand = task->sighand;
    spin_lock(&sighand->lock);
    old = sighand->actions[sig - 1];
    if (act) {
        sighand->actions[sig - 1] = new;
        if (signal_ignored(&new, sig))
            flush_proc_sigpending(task->proc, sigmask(sig));
    }
    spin_unlock(&sighand->lock);
    spin_unlock(&task->proc->lock);

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

    spin_lock(&task->proc->lock);
    old = task->blocked;

    if (set) {
        switch (how) {
        case SIG_BLOCK:
            WRITE_ONCE(task->blocked, old | new);
            break;
        case SIG_UNBLOCK:
            WRITE_ONCE(task->blocked, old & ~new);
            break;
        case SIG_SETMASK:
            WRITE_ONCE(task->blocked, new);
            break;
        default:
            spin_unlock(&task->proc->lock);
            return -EINVAL;
        }
        recalc_sigpending(task);
    }

    spin_unlock(&task->proc->lock);

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

    spin_lock(&task->proc->lock);
    ret = (task->pending.signal | task->proc->pending.signal) & task->blocked;
    spin_unlock(&task->proc->lock);

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

    spin_lock(&task->proc->lock);
    if (new) {
        old = task->sighand;
        sighand_copy(new, old);
        task->sighand = new;
    }
    task->sigstack = (stack_t){.ss_flags = SS_DISABLE};
    spin_lock(&task->sighand->lock);
    for (unsigned int i = 0; i < SIG_MAX; ++i) {
        struct sigaction *action = &task->sighand->actions[i];

        if (action->sa_handler != SIG_IGN)
            action->sa_handler = SIG_DFL;
        action->sa_flags = 0;
        action->sa_mask = 0;
    }
    spin_unlock(&task->sighand->lock);
    spin_unlock(&task->proc->lock);
    sighand_put(old);
}
