#include <nuvix/task.h>
#include <nuvix/fdtable.h>
#include <nuvix/mm.h>
#include <nuvix/page.h>
#include <nuvix/slab.h>
#include <nuvix/pid.h>
#include <nuvix/proc.h>
#include <nuvix/layout.h>
#include <nuvix/assert.h>
#include <nuvix/preempt.h>
#include <nuvix/sched.h>
#include <nuvix/vfs.h>
#include <nuvix/syscall.h>
#include <nuvix/wait.h>
#include <nuvix/uaccess.h>
#include <nuvix/string.h>
#include <nuvix/path.h>
#include <nuvix/completion.h>
#include <nuvix/futex.h>
#include <asm/trap.h>

static_assert(KSTACK_ORDER < CONFIG_MAX_ORDER,
              "kernel task stack order must be smaller than MAX_ORDER");

static DEFINE_WAIT_QUEUE(child_wait);

#define WNOHANG              1
#define WSTOPPED             2
#define WEXITED              4
#define WCONTINUED           8
#define WNOWAIT              0x01000000

#define P_ALL                0
#define P_PID                1
#define P_PGID               2

extern char boot_stack[];

struct task boot_task = {
    .refcount = REFCOUNT_INIT(1),
    .cpu_id = 0,
    .state = TASK_NEW,
    .on_cpu = true,
    .rq_node = LIST_INIT_HEAD(boot_task.rq_node),
    .thread = LIST_INIT_HEAD(boot_task.thread),
    .preempt_count = INIT_PREEMPT_COUNT,
    .flags = ATOMIC_INIT(0),
    .kstack = boot_stack,
};

static struct task *task_new(void)
{
    struct task *task __free(kfree) = NULL;
    struct page *page __free(kstack) = NULL;

    task = kzalloc(sizeof(*task));
    if (!task)
        return NULL;

    page = alloc_pages(KSTACK_ORDER);
    if (!page)
        return NULL;

    refcount_set(&task->refcount, 1);
    task->cpu_id = TASK_CPU_NONE;
    task->state = TASK_NEW;
    spin_lock_init(&task->lock);
    INIT_LIST_HEAD(&task->rq_node);
    INIT_LIST_HEAD(&task->thread);
    task->preempt_count = INIT_PREEMPT_COUNT;
    task->kstack = (void *)__va(page_to_phys(move(page)));
    task->ctx.sp = (vaddr_t)task->kstack + KSTACK_SIZE;
    return move(task);
}

struct task *ktask_alloc(int (*fn)(void *), void *arg)
{
    struct task *task = task_new();

    if (task)
        arch_ktask_init(&task->ctx, fn, arg);
    return task;
}

struct task *task_alloc(void)
{
    struct task *task = task_new();

    if (task) {
        signal_task_init(task);
        task_set_flag(task, TF_FORKNOEXEC);
    }
    return task;
}

void task_get(struct task *task)
{
    refcount_inc(&task->refcount);
}

static void free_resource(struct task *task)
{
    struct fdtable *fdt = move(task->fdtable);
    struct fs_struct *fs = move(task->fs);
    struct mm_struct *mm = move(task->mm);

    fdtable_put(fdt);
    fs_put(fs);
    mm_put(mm);
}

void task_free(struct task *task)
{
    Assert(task->state == TASK_NEW, "cannot discard a published task\n");
    pid_discard(task);
    if (task->proc)
        free_resource(task);
    free_pages(phys_to_page(__pa((vaddr_t)task->kstack)), KSTACK_ORDER);
    task->kstack = NULL;
    task_put(task);
}

static bool proc_waitable(struct proc *proc)
{
    bool empty;

    assert_spin_locked(&child_wait.lock);
    spin_lock(&proc->lock);
    empty = list_empty(&proc->threads);
    spin_unlock(&proc->lock);

    return !proc->leader->kstack && proc->leader->state == TASK_ZOMBIE && empty;
}

static int proc_status(const struct proc *proc)
{
    assert_spin_locked(&child_wait.lock);
    if (atomic_read_acquire(&proc->exiting))
        return proc->status;
    return proc->leader->exit;
}

static void notify_parent(struct proc *proc)
{
    siginfo_t info = {0};
    int status = proc_status(proc);

    info.si_signo = SIGCHLD;
    info.si_pid = task_pid_nr(proc->leader, PIDTYPE_TGID);
    info.si_code = (status & 0x7f) ? CLD_KILLED : CLD_EXITED;
    info.si_status = (status & 0x7f) ? status & 0x7f : status >> 8;
    send_signal(proc->parent, NULL, &info);
    wake_up_all_locked(&child_wait);
}

static void reparent_children(struct proc *proc)
{
    struct proc *child, *next;
    struct proc *reaper = init->proc;

    assert_spin_locked(&child_wait.lock);
    list_for_each_entry_safe (child, next, &proc->children, sibling) {
        pid_reparent(child, reaper);
        list_move_tail(&child->sibling, &reaper->children);
        if (proc_waitable(child))
            notify_parent(child);
    }
}

void task_vfork_done(struct task *task)
{
    spin_lock(&task->lock);
    struct completion *done = move(task->vfork_done);

    if (done)
        complete(done);
    spin_unlock(&task->lock);
}

void task_mm_release(void)
{
    uint32_t *addr;

    IFDEF(CONFIG_FUTEX, futex_release());
    addr = move(current->clear_child_tid);
    if (addr && refcount_read(&current->mm->urefs) > 1) {
        uint32_t zero = 0;
        size_t nocopied = copy_to_user(addr, &zero, sizeof(zero));
        (void)nocopied;
        IFDEF(CONFIG_FUTEX, (void)futex_wake(addr, 1));
    }
}

__noreturn void task_exit(int status)
{
    struct task *task = current;

    might_sleep();
    task->exit = status;
    if (task->proc) {
        Assert(task != init, "init cannot exit\n");
        spin_lock(&task->proc->lock);
        task_set_flag(task, TF_EXITING);
        recalc_sigpending(task);
        spin_unlock(&task->proc->lock);
        task_mm_release();
        free_resource(task);
        task_vfork_done(task);
    }
    preempt_disable();
    sched_exit();
}

static void start_group_exit(struct proc *proc, int status)
{
    struct task *task;

    assert_spin_locked(&proc->lock);
    if (atomic_read(&proc->exiting))
        return;
    proc->status = status;
    atomic_set_release(&proc->exiting, 1);
    for_each_thread (proc, task) {
        task_clear_flag(task, TF_STOPPENDING);
        if (task != current)
            sched_wake_group_exit(task);
    }
}

void proc_kill(struct proc *proc)
{
    spin_lock(&proc->lock);
    if (!list_empty(&proc->threads))
        start_group_exit(proc, SIGKILL);
    spin_unlock(&proc->lock);
}

__noreturn void task_exit_group(int status)
{
    struct proc *proc = current->proc;

    spin_lock(&proc->lock);
    if (atomic_read(&proc->exiting))
        status = proc->status;
    else
        start_group_exit(proc, status);
    spin_unlock(&proc->lock);
    task_exit(status);
}

void task_finish_exit(struct task *task)
{
    Assert(task->state == TASK_ZOMBIE && !task->on_cpu && !task->on_rq &&
               list_empty(&task->rq_node) && !task->mm && !task->fdtable,
           "task exit requires completed resource cleanup and switch\n");
    free_pages(phys_to_page(__pa((vaddr_t)task->kstack)), KSTACK_ORDER);
    if (task->proc) {
        unsigned long flags;

        spin_lock_irqsave(&child_wait.lock, flags);
        task->kstack = NULL;
        if (proc_remove_thread(task)) {
            reparent_children(task->proc);
            notify_parent(task->proc);
        }
        spin_unlock_irqrestore(&child_wait.lock, flags);
    } else {
        task->kstack = NULL;
    }
    task_put(task);
}

void task_put(struct task *task)
{
    if (!refcount_dec_and_test(&task->refcount))
        return;
    if (task->proc)
        signal_task_free(task);
    pid_put(task->tid);
    proc_put(task->proc);
    kfree(task);
}

long sys_exit(struct trap_frame *frame)
{
    task_exit((syscallarg(frame, 0) & 0xff) << 8);
}

long sys_exit_group(struct trap_frame *frame)
{
    task_exit_group((syscallarg(frame, 0) & 0xff) << 8);
}

long sys_set_tid_address(struct trap_frame *frame)
{
    current->clear_child_tid = (uint32_t *)syscallarg(frame, 0);
    return task_pid_nr(current, PIDTYPE_TID);
}

static int copy_proc(struct task *child)
{
    child->proc = proc_alloc(child);
    if (!child->proc)
        return -ENOMEM;
    return 0;
}

static int copy_mm(struct task *child, unsigned long uflags)
{
    if (uflags & CLONE_VM) {
        mm_get(current->mm);
        child->mm = current->mm;
        return 0;
    }
    child->mm = mm_alloc();
    if (!child->mm)
        return -ENOMEM;
    return mm_clone(child->mm, current->mm);
}

static int copy_files(struct task *child, unsigned long uflags)
{
    if (uflags & CLONE_FILES) {
        fdtable_get(current->fdtable);
        child->fdtable = current->fdtable;
    } else {
        child->fdtable = fdtable_clone(current->fdtable);
        if (!child->fdtable)
            return -ENOMEM;
    }
    if (uflags & CLONE_FS) {
        fs_get(current->fs);
        child->fs = current->fs;
    } else {
        child->fs = fs_clone(current->fs);
        if (!child->fs)
            return -ENOMEM;
    }
    return 0;
}

static int copy_signals(struct task *child, unsigned long uflags)
{
    if (uflags & CLONE_SIGHAND) {
        sighand_get(current->sighand);
        child->sighand = current->sighand;
    } else {
        child->sighand = sighand_clone(current->sighand);
        if (!child->sighand)
            return -ENOMEM;
    }
    spin_lock(&current->proc->lock);
    child->blocked = current->blocked;
    if (!(uflags & CLONE_VM) || (uflags & CLONE_VFORK))
        child->sigstack = current->sigstack;
    spin_unlock(&current->proc->lock);
    return 0;
}

static int copy_task(struct task *child, unsigned long flags)
{
    int ret = copy_proc(child);

    if (ret)
        return ret;
    ret = copy_mm(child, flags);
    if (ret)
        return ret;
    ret = copy_files(child, flags);
    if (ret)
        return ret;
    spin_lock(&current->lock);
    memcpy(child->name, current->name, sizeof(child->name));
    spin_unlock(&current->lock);
    ret = copy_signals(child, flags);
    if (ret)
        return ret;
    return pid_prepare(child, current);
}

static int publish_child(struct task *child)
{
    unsigned long flags;
    int ret;

    spin_lock_irqsave(&child_wait.lock, flags);
    if (child->proc != current->proc) {
        pid_reparent(child->proc, current->proc);
        list_add_tail(&child->proc->sibling, &current->proc->children);
    }
    ret = proc_add_thread(child);
    if (ret && child->proc != current->proc) {
        list_del_init(&child->proc->sibling);
        pid_reparent(child->proc, NULL);
    }
    spin_unlock_irqrestore(&child_wait.lock, flags);
    if (!ret)
        sched_publish(child);
    return ret;
}

long sys_clone(struct trap_frame *frame)
{
    unsigned long uflags = syscallarg(frame, 0);
    struct task *child __free(task) = NULL;
    struct task *vfork_child __free(task_put) = NULL;
    DEFINE_COMPLETION(done);
    pid_t pid;
    int ret;

    if ((uflags & CLONE_SIGHAND) && !(uflags & CLONE_VM))
        return -EINVAL;
    child = task_alloc();
    if (!child)
        return -ENOMEM;
    ret = copy_task(child, uflags);
    if (ret)
        return ret;
    arch_task_clone(child);
    pid = task_pid_nr(child, PIDTYPE_TID);
    if (uflags & CLONE_PARENT_SETTID) {
        pid_t *parent_tid = (pid_t *)syscallarg(frame, 2);
        size_t nocopied = copy_to_user(parent_tid, &pid, sizeof(pid));
        (void)nocopied;
    }
    if (uflags & CLONE_CHILD_CLEARTID)
        child->clear_child_tid = (uint32_t *)syscallarg(frame, 4);
    if (uflags & CLONE_VFORK) {
        child->vfork_done = &done;
        task_get(child);
        vfork_child = child;
    }
    ret = publish_child(child);
    if (ret)
        return ret;
    keep(child);
    if (vfork_child) {
        (void)wait_for_completion_killable(&done);
        spin_lock(&vfork_child->lock);
        vfork_child->vfork_done = NULL;
        spin_unlock(&vfork_child->lock);
    }
    return pid;
}

struct wait_event {
    pid_t pid;
    int exit;
};

enum wait_mode {
    WAIT_ALL,
    WAIT_PID,
    WAIT_PGID,
};

static bool wait_matches(enum wait_mode mode, pid_t id,
                         const struct task *child)
{
    if (mode == WAIT_PID)
        return task_pid_nr(child, PIDTYPE_TGID) == id;
    if (mode == WAIT_PGID)
        return task_pid_nr(child, PIDTYPE_PGID) == id;
    return true;
}

static long wait_child(enum wait_mode mode, pid_t id, int options,
                       struct wait_event *event)
{
    struct proc *child;
    struct task *dead = NULL;
    struct wait_entry wait;
    unsigned long flags;
    long ret;

    wait_entry_init(&wait);
    for (;;) {
        preempt_disable();
        spin_lock_irqsave(&child_wait.lock, flags);
        finish_wait_locked(&child_wait, &wait);
        ret = -ECHILD;
        list_for_each_entry (child, &current->proc->children, sibling) {
            struct task *leader = child->leader;

            if (!wait_matches(mode, id, leader))
                continue;
            ret = 0;
            if (!proc_waitable(child))
                continue;
            event->pid = task_pid_nr(leader, PIDTYPE_TGID);
            event->exit = proc_status(child);
            ret = event->pid;
            if (!(options & WNOWAIT)) {
                dead = leader;
                pid_unpublish(leader);
                list_del_init(&child->sibling);
                pid_reparent(child, NULL);
            }
            break;
        }
        if (ret || (options & WNOHANG))
            break;
        if (signal_pending(current)) {
            ret = -EINTR;
            break;
        }
        prepare_to_wait_locked(&child_wait, &wait, TASK_INTERRUPTIBLE);
        spin_unlock_irqrestore(&child_wait.lock, flags);
        sched_park();
    }
    spin_unlock_irqrestore(&child_wait.lock, flags);
    preempt_enable();
    if (dead)
        task_put(dead);
    return ret;
}

long sys_wait4(struct trap_frame *frame)
{
    pid_t pid = (pid_t)syscallarg(frame, 0);
    int *status = (int *)syscallarg(frame, 1);
    int options = (int)syscallarg(frame, 2);
    void *rusage = (void *)syscallarg(frame, 3);
    struct wait_event event = {0};
    long ret;

    if ((pid != -1 && pid <= 0) || (options & ~(WNOHANG | WSTOPPED)))
        return -EINVAL;
    if (rusage)
        return -EOPNOTSUPP;

    ret = wait_child(pid == -1 ? WAIT_ALL : WAIT_PID, pid, options, &event);
    if (ret > 0 && status &&
        copy_to_user(status, &event.exit, sizeof(event.exit)))
        return -EFAULT;
    return ret;
}

long sys_waitid(struct trap_frame *frame)
{
    int which = (int)syscallarg(frame, 0);
    pid_t id = (pid_t)syscallarg(frame, 1);
    siginfo_t *infop = (siginfo_t *)syscallarg(frame, 2);
    int options = (int)syscallarg(frame, 3);
    void *rusage = (void *)syscallarg(frame, 4);
    enum wait_mode mode;
    struct wait_event event = {0};
    siginfo_t info = {0};
    long ret;

    if (options & ~(WNOHANG | WSTOPPED | WEXITED | WCONTINUED | WNOWAIT))
        return -EINVAL;
    if (options & (WSTOPPED | WCONTINUED))
        return -ENOTSUPP;
    if (!(options & WEXITED))
        return -EINVAL;
    if (rusage)
        return -EOPNOTSUPP;
    switch (which) {
    case P_PID:
        mode = WAIT_PID;
        break;
    case P_PGID:
        mode = WAIT_PGID;
        break;
    case P_ALL:
        mode = WAIT_ALL;
        break;
    default:
        return -EINVAL;
    }
    if (mode == WAIT_PID && id <= 0)
        return -EINVAL;
    if (mode == WAIT_PGID && id < 0)
        return -EINVAL;
    if (mode == WAIT_PGID && !id)
        id = task_pid_nr(current, PIDTYPE_PGID);

    ret = wait_child(mode, id, options, &event);
    if (ret > 0) {
        info.si_signo = SIGCHLD;
        info.si_pid = event.pid;
        info.si_code = (event.exit & 0x7f) ? CLD_KILLED : CLD_EXITED;
        info.si_status =
            (event.exit & 0x7f) ? event.exit & 0x7f : event.exit >> 8;
    }
    if (infop && copy_to_user(infop, &info, sizeof(info)))
        return -EFAULT;
    return ret > 0 ? 0 : ret;
}
