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
#include <nuvix/syscall.h>
#include <asm/trap.h>

void signal_task_init(struct task *task)
{
    spin_lock_init(&task->signal_lock);
    INIT_LIST_HEAD(&task->pending.list);
    task->pending.signal = 0;
    task->blocked = 0;
    for (unsigned int i = 0; i < SIG_MAX; ++i)
        task->actions[i] = (struct sigaction){.sa_handler = SIG_DFL};
}

void signal_task_free(struct task *task)
{
    struct sigqueue *queue, *next;

    list_for_each_entry_safe (queue, next, &task->pending.list, list) {
        struct sigqueue *entry __free(kfree) = queue;

        list_del_init(&entry->list);
    }
    task->pending.signal = 0;
}

int send_signal(struct task *task, const siginfo_t *info)
{
    struct sigqueue *queue __free(kfree) = NULL;
    int sig = info->si_signo;

    if (!sig)
        return 0;

    queue = kmalloc(sizeof(*queue));
    if (!queue)
        return -ENOMEM;

    queue->info = *info;

    spin_lock(&task->signal_lock);
    if (sig_valid(task, sig)) {
        list_add_tail(&queue->list, &task->pending.list);
        task->pending.signal |= sigmask(sig);
        keep(queue);
    }
    spin_unlock(&task->signal_lock);

    return 0;
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

    task = pid_get_task(pid, PIDTYPE_TGID);
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

    task = pid_get_task(tid, PIDTYPE_TID);
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

    task = pid_get_task(tid, PIDTYPE_TID);
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
