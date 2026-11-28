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
    struct poll_entry *entries;
    unsigned int count;
    uint64_t deadline;
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

static int poll_initwait(struct poll_sleep *sleep, unsigned int count,
                         uint64_t deadline)
{
    sleep->count = count;
    sleep->task = current;
    sleep->deadline = deadline;
    if (count) {
        sleep->entries = kzalloc(count * sizeof(*sleep->entries));
        if (!sleep->entries)
            return -ENOMEM;
        for (unsigned int i = 0; i < count; ++i)
            wait_entry_init(&sleep->entries[i].wait);
    }
    if (deadline != UINT64_MAX) {
        timer_init(&sleep->timer, current_cpu_id(), poll_timeout);
        timer_start(&sleep->timer, deadline);
    }
    return 0;
}

static void poll_prepare(void)
{
    struct rq *rq;
    unsigned long flags;

    preempt_disable();
    rq = this_rq();
    spin_lock_irqsave(&rq->lock, flags);
    sched_set_sleep_locked(rq, TASK_INTERRUPTIBLE);
    spin_unlock_irqrestore(&rq->lock, flags);
}

static bool poll_sched(struct poll_sleep *sleep, int *result)
{
    if (!*result) {
        if (signal_pending(sleep->task))
            *result = -EINTR;
        else if (time_read() < sleep->deadline) {
            sched_park();
            return true;
        }
    }
    sched_wake(sleep->task);
    preempt_enable();
    return false;
}

static void poll_freewait(struct poll_sleep *sleep)
{
    unsigned long flags;

    if (sleep->deadline != UINT64_MAX)
        timer_cancel_sync(&sleep->timer);
    for (unsigned int i = 0; i < sleep->count; ++i) {
        struct poll_entry *entry = &sleep->entries[i];

        if (entry->queue) {
            spin_lock_irqsave(&entry->queue->lock, flags);
            finish_wait_locked(entry->queue, &entry->wait);
            spin_unlock_irqrestore(&entry->queue->lock, flags);
        }
        fput(entry->file);
    }
    kfree(sleep->entries);
}

static int do_poll(struct pollfd *fds, unsigned int nfds, uint64_t deadline)
{
    struct poll_sleep sleep = {0};
    int ret = poll_initwait(&sleep, nfds, deadline);

    if (ret)
        return ret;
    for (unsigned int i = 0; i < nfds; ++i)
        sleep.entries[i].file = fd_open(current->fdtable, fds[i].fd);
    do {
        ret = 0;
        poll_prepare();
        for (unsigned int i = 0; i < nfds; ++i) {
            struct poll_entry *entry = &sleep.entries[i];
            struct poll_table table = {.entry = entry};

            if (fds[i].fd < 0)
                fds[i].revents = 0;
            else if (!entry->file)
                fds[i].revents = POLLNVAL;
            else
                fds[i].revents = poll_file(entry->file, &table) &
                                 (fds[i].events | POLLERR | POLLHUP);
            ret += fds[i].revents != 0;
        }
    } while (poll_sched(&sleep, &ret));
    poll_freewait(&sleep);
    return ret;
}

static int do_select(unsigned int nfds, const unsigned long requested[3],
                     unsigned long ready[3], uint64_t deadline)
{
    const short events[] = {POLLIN | POLLERR | POLLHUP, POLLOUT | POLLERR,
                            POLLPRI};
    unsigned long selected = requested[0] | requested[1] | requested[2];
    struct poll_sleep sleep = {0};
    int ret;

    if (nfds > BITS_PER_LONG)
        nfds = BITS_PER_LONG;
    if (nfds < BITS_PER_LONG)
        selected &= (1UL << nfds) - 1;
    ret = poll_initwait(&sleep, selected ? nfds : 0, deadline);
    if (ret)
        return ret;
    for (unsigned int fd = 0; fd < sleep.count; ++fd) {
        if (selected & (1UL << fd))
            sleep.entries[fd].file = fd_open(current->fdtable, fd);
    }
    do {
        ret = 0;
        poll_prepare();
        for (unsigned int fd = 0; fd < sleep.count; ++fd) {
            unsigned long bit = 1UL << fd;
            struct poll_entry *entry = &sleep.entries[fd];
            struct poll_table table = {.entry = entry};
            poll_t mask;

            if (!(selected & bit))
                continue;
            if (!entry->file) {
                ret = -EBADF;
                break;
            }
            mask = poll_file(entry->file, &table);
            for (int i = 0; i < 3; ++i) {
                if ((requested[i] & bit) && (mask & events[i])) {
                    ready[i] |= bit;
                    ++ret;
                }
            }
        }
    } while (poll_sched(&sleep, &ret));
    poll_freewait(&sleep);
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
    spin_lock(&task->proc->lock);
    *saved = task->blocked;
    WRITE_ONCE(task->blocked, set & ~SIG_UNBLOCKMASK);
    recalc_sigpending(task);
    spin_unlock(&task->proc->lock);
    return 0;
}

static void poll_restore_sigmask(sigset_t saved, bool interrupted)
{
    struct task *task = current;

    spin_lock(&task->proc->lock);
    if (interrupted) {
        task->sigmask = saved;
        task_set_flag(task, TF_RESIGMASK);
    } else {
        WRITE_ONCE(task->blocked, saved);
        recalc_sigpending(task);
    }
    spin_unlock(&task->proc->lock);
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

long sys_pselect6(struct trap_frame *frame)
{
    int nfds = syscallarg(frame, 0);
    unsigned long *sets[] = {
        (unsigned long *)syscallarg(frame, 1),
        (unsigned long *)syscallarg(frame, 2),
        (unsigned long *)syscallarg(frame, 3),
    };
    struct timespec64 *utmo = (struct timespec64 *)syscallarg(frame, 4);
    const void *uarg = (const void *)syscallarg(frame, 5);
    struct {
        const sigset_t *mask;
        size_t size;
    } arg = {0};
    unsigned long requested[3] = {0}, ready[3] = {0};
    uint64_t deadline;
    sigset_t saved = 0;
    int ret;

    if (nfds < 0)
        return -EINVAL;
    if (uarg && copy_from_user(&arg, uarg, sizeof(arg)))
        return -EFAULT;
    ret = poll_deadline(utmo, &deadline);
    if (ret)
        return ret;
    for (int i = 0; i < 3; ++i)
        if (nfds && sets[i] &&
            copy_from_user(&requested[i], sets[i], sizeof(unsigned long)))
            return -EFAULT;
    ret = poll_swap_sigmask(arg.mask, arg.size, &saved);
    if (ret)
        return ret;
    ret = do_select(nfds, requested, ready, deadline);
    if (arg.mask)
        poll_restore_sigmask(saved, ret == -EINTR);
    if (utmo && deadline != UINT64_MAX && (ret >= 0 || ret == -EINTR || ret == -EBADF)) {
        struct timespec64 remaining;
        uint64_t now = time_read();

        time_timespec_from_ticks(now < deadline ? deadline - now : 0, &remaining);

        /* bypass around the `unused-result` check of gcc */
        int tmp = copy_to_user(utmo, &remaining, sizeof(remaining));
        (void)tmp;
    }
    if (ret < 0)
        return ret;
    for (int i = 0; i < 3; ++i)
        if (nfds && sets[i] &&
            copy_to_user(sets[i], &ready[i], sizeof(unsigned long)))
            return -EFAULT;
    return ret;
}
