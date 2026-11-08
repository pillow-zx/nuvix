#include <nuvix/poll.h>
#include <nuvix/bitmap.h>
#include <nuvix/ctypes.h>
#include <nuvix/errno.h>
#include <nuvix/fdtable.h>
#include <nuvix/fs.h>
#include <nuvix/list.h>
#include <nuvix/macro.h>
#include <nuvix/preempt.h>
#include <nuvix/sched.h>
#include <nuvix/signal.h>
#include <nuvix/slab.h>
#include <nuvix/spinlock.h>
#include <nuvix/syscall.h>
#include <nuvix/task.h>
#include <nuvix/tick.h>
#include <nuvix/timer.h>
#include <nuvix/uaccess.h>
#include <nuvix/wait.h>
#include <asm/trap.h>

struct poll_entry {
    struct wait_entry wait;
    struct wait_queue *queue;
    struct file *file;
};

struct poll_sleep {
    struct timer timer;
    struct task *task;
};

static void poll_timeout(struct timer *timer)
{
    struct poll_sleep *sleep = container_of(timer, struct poll_sleep, timer);

    sched_wake(sleep->task);
}

void poll_add(struct wait_queue *queue, struct poll_table *table)
{
    struct poll_entry *entry = table->entry;

    assert_spin_locked(&queue->lock);
    Assert(!entry->queue || entry->queue == queue,
           "poll entry requeued on a different queue\n");
    entry->queue = queue;
    if (list_empty(&entry->wait.node))
        list_add_tail(&entry->wait.node, &queue->waiters);
}

static poll_t poll_file(struct file *file, struct poll_table *table)
{
    if (file->f_ops->poll)
        return file->f_ops->poll(file, table);
    return (FILE_READ(file) ? POLLIN : 0) | (FILE_WRITE(file) ? POLLOUT : 0);
}

static int poll_wait(struct pollfd *fds, struct poll_entry *entries,
                     unsigned int nfds, uint64_t deadline)
{
    struct task *task = current;
    struct poll_table table;
    struct rq *rq;
    unsigned long flags;

    for (;;) {
        unsigned int ready = 0;

        preempt_disable();
        rq = this_rq();
        spin_lock_irqsave(&rq->lock, flags);
        sched_set_sleep_locked(rq, TASK_INTERRUPTIBLE);
        spin_unlock_irqrestore(&rq->lock, flags);
        for (unsigned int i = 0; i < nfds; ++i) {
            struct poll_entry *entry = &entries[i];
            poll_t mask;

            if (fds[i].fd < 0) {
                fds[i].revents = 0;
                continue;
            }
            if (!entry->file) {
                fds[i].revents = POLLNVAL;
                ++ready;
                continue;
            }
            table.entry = entry;
            mask = poll_file(entry->file, &table);
            mask &= fds[i].events | POLLERR | POLLHUP;
            fds[i].revents = (short)mask;
            ready += mask != 0;
        }
        if (!ready && !signal_pending(task) && time_read() < deadline) {
            sched_park();
            continue;
        }
        (void)sched_wake(task);
        preempt_enable();
        if (ready)
            return (int)ready;
        if (signal_pending(task))
            return -EINTR;
        if (time_read() >= deadline)
            return 0;
    }
}

static int do_poll(struct pollfd *fds, unsigned int nfds, uint64_t deadline)
{
    struct poll_entry *entries __free(kfree) = NULL;
    struct poll_sleep sleep = {.task = current};
    bool timed = deadline != UINT64_MAX;
    unsigned long flags;
    int ret;

    if (nfds) {
        entries = kzalloc(nfds * sizeof(*entries));
        if (!entries)
            return -ENOMEM;
        for (unsigned int i = 0; i < nfds; ++i) {
            wait_entry_init(&entries[i].wait);
            entries[i].file = fd_open(current->fdtable, fds[i].fd);
        }
    }
    if (timed) {
        timer_init(&sleep.timer, current_cpu_id(), poll_timeout);
        timer_start(&sleep.timer, deadline);
    }

    ret = poll_wait(fds, entries, nfds, deadline);

    if (timed)
        timer_cancel_sync(&sleep.timer);
    for (unsigned int i = 0; i < nfds; ++i) {
        if (entries[i].queue) {
            spin_lock_irqsave(&entries[i].queue->lock, flags);
            finish_wait_locked(entries[i].queue, &entries[i].wait);
            spin_unlock_irqrestore(&entries[i].queue->lock, flags);
        }
        fput(entries[i].file);
    }
    return ret;
}

static int poll_deadline(const struct timespec64 *utmo, uint64_t *deadline)
{
    struct timespec64 ts;
    uint64_t ticks;
    int ret;

    *deadline = UINT64_MAX;
    if (!utmo)
        return 0;
    if (copy_from_user(&ts, utmo, sizeof(ts)))
        return -EFAULT;
    ret = time_ticks_from_timespec(&ts, &ticks);
    if (ret == -EOVERFLOW)
        ticks = UINT64_MAX;
    else if (ret)
        return ret;
    if (check_add_overflow(time_read(), ticks, deadline))
        *deadline = UINT64_MAX;
    return 0;
}

static int poll_swap_sigmask(const sigset_t *umask, size_t sigsetsize,
                             sigset_t *saved)
{
    struct task *task = current;
    sigset_t set;

    if (!umask)
        return 0;
    if (sigsetsize != SIGSET_SIZE)
        return -EINVAL;
    if (copy_from_user(&set, umask, sizeof(set)))
        return -EFAULT;
    spin_lock(&task->signal_lock);
    *saved = task->blocked;
    task->blocked = set & ~SIG_UNBLOCKMASK;
    recalc_sigpending(task);
    spin_unlock(&task->signal_lock);
    return 0;
}

static void poll_restore_sigmask(sigset_t saved, bool interrupted)
{
    struct task *task = current;

    spin_lock(&task->signal_lock);
    if (interrupted) {
        task->sigmask = saved;
        task_set_flag(task, TF_RESIGMASK);
    } else {
        task->blocked = saved;
        recalc_sigpending(task);
    }
    spin_unlock(&task->signal_lock);
}

long sys_ppoll(struct trap_frame *frame)
{
    const struct pollfd *ufds = (const struct pollfd *)syscallarg(frame, 0);
    unsigned int nfds = syscallarg(frame, 1);
    const struct timespec64 *utmo = (const struct timespec64 *)syscallarg(frame, 2);
    const sigset_t *umask = (const sigset_t *)syscallarg(frame, 3);
    size_t sigsetsize = syscallarg(frame, 4);
    struct pollfd fds[BITS_PER_LONG];
    sigset_t saved = 0;
    uint64_t deadline;
    int ret;

    if (nfds > BITS_PER_LONG)
        return -EINVAL;
    if (nfds && copy_from_user(fds, ufds, nfds * sizeof(*fds)))
        return -EFAULT;
    ret = poll_deadline(utmo, &deadline);
    if (ret)
        return ret;
    ret = poll_swap_sigmask(umask, sigsetsize, &saved);
    if (ret)
        return ret;

    ret = do_poll(fds, nfds, deadline);

    if (umask)
        poll_restore_sigmask(saved, ret == -EINTR);
    if (ret >= 0 && nfds &&
        copy_to_user((struct pollfd *)ufds, fds, nfds * sizeof(*fds)))
        ret = -EFAULT;
    return ret;
}

