#include <nuvix/sleep.h>
#include <nuvix/syscall.h>
#include <nuvix/completion.h>
#include <nuvix/timer.h>
#include <nuvix/tick.h>
#include <nuvix/errno.h>
#include <nuvix/uaccess.h>
#include <asm/trap.h>

struct sleep {
    struct timer timer;
    struct completion done;
};

static void sleep_complete(struct timer *timer)
{
    struct sleep *waiter =
        container_of(timer, struct sleep, timer);

    complete(&waiter->done);
}

int sleep_until(uint64_t deadline)
{
    struct sleep waiter;
    int ret;

    might_sleep();
    if (deadline <= time_read())
        return 0;
    init_completion(&waiter.done);
    timer_init(&waiter.timer, current_cpu_id(), sleep_complete);
    ret = timer_start(&waiter.timer, deadline);
    if (ret)
        return ret;
    ret = wait_for_completion_interruptible(&waiter.done);
    timer_cancel_sync(&waiter.timer);
    return ret == -ERESTARTSYS ? -EINTR : ret;
}

int sleep_ns(uint64_t ns)
{
    uint64_t deadline;
    int ret;

    might_sleep();
    if (!ns)
        return 0;
    ret = time_deadline_from_ns(ns, &deadline);
    if (ret)
        return ret;
    return sleep_until(deadline);
}

long sys_nanosleep(struct trap_frame *frame)
{
    const struct timespec64 *rqtp =
        (const struct timespec64 *)syscallarg(frame, 0);
    struct timespec64 *rmtp = (struct timespec64 *)syscallarg(frame, 1);
    struct timespec64 ts;
    uint64_t ticks, deadline, now;
    int ret;

    if (copy_from_user(&ts, rqtp, sizeof(ts)))
        return -EFAULT;
    ret = time_ticks_from_timespec(&ts, &ticks);
    if (ret == -EOVERFLOW)
        ticks = UINT64_MAX;
    else if (ret)
        return ret;
    if (check_add_overflow(time_read(), ticks, &deadline))
        deadline = UINT64_MAX;

    ret = sleep_until(deadline);
    if (ret != -EINTR || !rmtp)
        return ret;
    now = time_read();
    if (now >= deadline)
        return 0;
    time_timespec_from_ticks(deadline - now, &ts);
    if (copy_to_user(rmtp, &ts, sizeof(ts)))
        return -EFAULT;
    return -EINTR;
}

long sys_clock_nanosleep(struct trap_frame *frame)
{
    int clock_id = syscallarg(frame, 0);
    int flags = syscallarg(frame, 1);
    const struct timespec64 *request =
        (const struct timespec64 *)syscallarg(frame, 2);
    struct timespec64 *remaining = (struct timespec64 *)syscallarg(frame, 3);
    bool absolute = flags & TIMER_ABSTIME;
    struct timespec64 ts;
    uint64_t ticks, deadline, now;
    int ret;

    if (clock_id != CLOCK_MONOTONIC && (clock_id != CLOCK_REALTIME || absolute))
        return -EINVAL;
    if (copy_from_user(&ts, request, sizeof(ts)))
        return -EFAULT;
    ret = time_ticks_from_timespec(&ts, &ticks);
    if (ret == -EOVERFLOW)
        ticks = UINT64_MAX;
    else if (ret)
        return ret;
    deadline = ticks;
    if (!absolute && check_add_overflow(time_read(), ticks, &deadline))
        deadline = UINT64_MAX;

    ret = sleep_until(deadline);
    if (ret != -EINTR || absolute || !remaining)
        return ret;
    now = time_read();
    if (now >= deadline)
        return 0;
    time_timespec_from_ticks(deadline - now, &ts);
    if (copy_to_user(remaining, &ts, sizeof(ts)))
        return -EFAULT;
    return -EINTR;
}
