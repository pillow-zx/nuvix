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
static_assert(offsetof(struct task, kstack) == TASK_KSTACK,
              "invalid task stack offset");
static_assert(offsetof(struct task, ctx.ra) == TASK_CTX_RA,
              "invalid task context offset");
static_assert(offsetof(struct task, ctx.sum) == TASK_CTX_SUM,
              "invalid task SUM context offset");

static DEFINE_WAIT_QUEUE(child_wait);

#define WNOHANG     1
#define WSTOPPED    2
#define WEXITED     4
#define WCONTINUED  8
#define WNOWAIT     0x01000000

#define P_ALL       0
#define P_PID       1
#define P_PGID      2

#define CLONE_VM    0x00000100UL
#define CLONE_VFORK 0x00004000UL

struct task boot_task = {
    .refcount = REFCOUNT_INIT(1),
    .cpu_id = 0,
    .state = TASK_NEW,
    .on_cpu = true,
    .rq_node = LIST_INIT_HEAD(boot_task.rq_node),
    .children = LIST_INIT_HEAD(boot_task.children),
    .sibling = LIST_INIT_HEAD(boot_task.sibling),
    .preempt_count = INIT_PREEMPT_COUNT,
    .flags = ATOMIC_INIT(0),
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
    if (user) {
        signal_task_init(task);
        task_set_flag(task, TF_FORKNOEXEC);
    }
    return move(task);
}

void task_get(struct task *task)
{
    refcount_inc(&task->refcount);
}

static void free_resource(struct task *task)
{
    struct fdtable *fdt = move(task->fdtable);
    struct mm_struct *mm = move(task->mm);

    fdtable_destroy(fdt);
    path_put(&task->cwd);
    kfree(fdt);
    mm_put(mm);
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

static void notify_parent(struct task *task)
{
    siginfo_t info = {0};

    assert_spin_locked(&child_wait.lock);
    info.si_signo = SIGCHLD;
    info.si_pid = task_pid_nr(task, PIDTYPE_TGID);
    info.si_code = (task->exit & 0x7f) ? CLD_KILLED : CLD_EXITED;
    info.si_status = (task->exit & 0x7f) ? task->exit & 0x7f : task->exit >> 8;
    send_signal(task->parent, &info);
    wake_up_all_locked(&child_wait);
}

static void reparent_children(struct task *task)
{
    struct task *child, *next;
    unsigned long flags;

    spin_lock_irqsave(&child_wait.lock, flags);
    list_for_each_entry_safe (child, next, &task->children, sibling) {
        pid_reparent(child, init);
        list_move_tail(&child->sibling, &init->children);
        if (!child->kstack && child->state == TASK_ZOMBIE)
            notify_parent(child);
    }
    spin_unlock_irqrestore(&child_wait.lock, flags);
}

void task_vfork_done(struct task *task)
{
    spin_lock(&task->signal_lock);
    struct completion *done = move(task->vfork_done);

    if (done)
        complete(done);
    spin_unlock(&task->signal_lock);
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
    if (task->parent) {
        Assert(task != init, "init cannot exit\n");
        task_mm_release();
        free_resource(task);
        task_vfork_done(task);
        reparent_children(task);
    }
    preempt_disable();
    sched_exit();
}

void task_finish_exit(struct task *task)
{
    Assert(task->state == TASK_ZOMBIE && !task->on_cpu && !task->on_rq &&
               list_empty(&task->rq_node) && !task->mm && !task->fdtable,
           "task exit requires completed resource cleanup and switch\n");
    free_pages(phys_to_page(__pa((vaddr_t)task->kstack)), KSTACK_ORDER);
    if (task->parent) {
        unsigned long flags;

        spin_lock_irqsave(&child_wait.lock, flags);
        task->kstack = NULL;
        notify_parent(task);
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
    Assert((task->state == TASK_NEW || task->state == TASK_ZOMBIE) &&
               !task->on_cpu && !task->on_rq && list_empty(&task->rq_node) &&
               !task->kstack && !task->mm && !task->fdtable,
           "cannot destroy a task before resource and scheduler teardown\n");
    Assert((!task->parent || task->parent == task) &&
               list_empty(&task->children) && list_empty(&task->sibling),
           "cannot destroy a task linked in the process tree\n");
    if (task->parent)
        signal_task_free(task);
    for (enum pid_type type = 0; type < PIDTYPE_MAX; ++type) {
        Assert(hlist_unhashed(&task->pid_links[type]),
               "cannot destroy a task with published identities\n");
        struct pid *pid __free(pid_put) = move(task->pids[type]);
    }
    kfree(task);
}

long sys_exit(struct trap_frame *frame)
{
    task_exit((syscallarg(frame, 0) & 0xff) << 8);
}

long sys_exit_group(struct trap_frame *frame)
{
    task_exit((syscallarg(frame, 0) & 0xff) << 8);
}

long sys_set_tid_address(struct trap_frame *frame)
{
    current->clear_child_tid = (uint32_t *)syscallarg(frame, 0);
    return task_pid_nr(current, PIDTYPE_TID);
}

long sys_clone(struct trap_frame *frame)
{
    unsigned long uflags = syscallarg(frame, 0);
    unsigned long sp = syscallarg(frame, 1);
    struct task *child __free(task) = NULL;
    DEFINE_COMPLETION(done);
    unsigned long flags;
    pid_t pid;
    int ret;

    if (uflags != SIGCHLD && uflags != (CLONE_VM | SIGCHLD) &&
        uflags != (CLONE_VM | CLONE_VFORK | SIGCHLD))
        return -EINVAL;
    child = task_alloc(true);
    if (!child)
        return -ENOMEM;
    if (uflags & CLONE_VM) {
        struct mm_struct *mm __free(mm_put) = move(child->mm);

        mm_get(current->mm);
        child->mm = current->mm;
    } else {
        ret = mm_clone(child->mm, current->mm);
        if (ret)
            return ret;
    }
    fdtable_clone(child->fdtable, current->fdtable);
    child->cwd = current->cwd;
    path_get(&child->cwd);
    spin_lock(&current->signal_lock);
    child->blocked = current->blocked;
    if (!(uflags & CLONE_VM) || (uflags & CLONE_VFORK))
        child->sigstack = current->sigstack;
    memcpy(child->actions, current->actions, sizeof(child->actions));
    spin_unlock(&current->signal_lock);
    ret = pid_prepare(child, current);
    if (ret)
        return ret;
    arch_task_clone(child, frame, sp);
    pid = task_pid_nr(child, PIDTYPE_TGID);
    if (uflags & CLONE_VFORK)
        child->vfork_done = &done;
    spin_lock_irqsave(&child_wait.lock, flags);
    child->parent = current;
    list_add_tail(&child->sibling, &current->children);
    spin_unlock_irqrestore(&child_wait.lock, flags);
    sched_publish(child);
    if (uflags & CLONE_VFORK) {
        (void)wait_for_completion_killable(&done);
        spin_lock(&child->signal_lock);
        child->vfork_done = NULL;
        spin_unlock(&child->signal_lock);
    }
    keep(child);
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
    struct task *child, *dead = NULL;
    struct wait_entry wait;
    unsigned long flags;
    long ret;

    wait_entry_init(&wait);
    for (;;) {
        preempt_disable();
        spin_lock_irqsave(&child_wait.lock, flags);
        finish_wait_locked(&child_wait, &wait);
        ret = -ECHILD;
        list_for_each_entry (child, &current->children, sibling) {
            if (!wait_matches(mode, id, child))
                continue;
            ret = 0;
            if (child->kstack || child->state != TASK_ZOMBIE)
                continue;
            event->pid = task_pid_nr(child, PIDTYPE_TGID);
            event->exit = child->exit;
            ret = event->pid;
            if (!(options & WNOWAIT)) {
                dead = child;
                pid_unpublish(child);
                list_del_init(&child->sibling);
                child->parent = child;
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

    if ((pid != -1 && pid <= 0) || (options & ~WNOHANG))
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
