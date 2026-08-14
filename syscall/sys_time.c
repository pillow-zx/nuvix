/*
 * syscall/sys_time.c - clock and sleep syscall adapters
 */

#include <kernel/errno.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/resource.h>
#include <kernel/syscall.h>
#include <kernel/task.h>
#include <kernel/time.h>
#include <kernel/timer.h>
#include <kernel/trap.h>
#include <kernel/wait.h>

#define NSEC_PER_SEC 1000000000LL

static bool clock_id_supported(int clock_id)
{
	return clock_id == CLOCK_REALTIME || clock_id == CLOCK_MONOTONIC ||
	       clock_id == CLOCK_BOOTTIME;
}

static uint64_t clock_ticks_now(void)
{
	return timer_now() / CLOCKS_PER_TICK;
}

static int syscall_wait_deadline(const struct wait_deadline *deadline,
				 wait_outcome_t *outcome)
{
	struct task_wait *wait = &current_task()->wait;
	int ret;

	ret = wait_start(wait, WAIT_FLAG_INTERRUPTIBLE, deadline);
	if (ret == 0)
		ret = wait_block(wait, outcome);
	wait_finish(wait);
	return ret;
}

ssize_t sys_times(struct trap_frame *tf)
{
	struct tms *utms = (struct tms *)syscall_arg(tf, 0);
	struct proc_cputime_snapshot snapshot;
	struct tms ktms = {
		.tms_utime =
			cputime_to_clock_t(task_user_ticks(current_task())),
		.tms_stime =
			cputime_to_clock_t(task_system_ticks(current_task())),
	};

	proc_cputime_snapshot(current_task()->proc, &snapshot);
	ktms.tms_cutime = cputime_to_clock_t(snapshot.children.utime_ticks);
	ktms.tms_cstime = cputime_to_clock_t(snapshot.children.stime_ticks);

	if (utms && copy_to_user(utms, &ktms, sizeof(ktms)) != 0)
		return -EFAULT;
	return (ssize_t)cputime_to_clock_t(clock_ticks_now());
}

ssize_t sys_gettimeofday(struct trap_frame *tf)
{
	struct timeval *utv = (struct timeval *)syscall_arg(tf, 0);
	struct timezone *utz = (struct timezone *)syscall_arg(tf, 1);
	struct timespec kts;
	struct timeval ktv;

	if (utv) {
		kernel_realtime_now(&kts);
		ktv.tv_sec = kts.tv_sec;
		ktv.tv_usec = kts.tv_nsec / 1000;
		if (copy_to_user(utv, &ktv, sizeof(ktv)) != 0)
			return -EFAULT;
	}
	if (utz) {
		struct timezone ktz = {0};

		if (copy_to_user(utz, &ktz, sizeof(ktz)) != 0)
			return -EFAULT;
	}
	return 0;
}

ssize_t sys_clock_gettime(struct trap_frame *tf)
{
	int clock_id = (int)syscall_arg(tf, 0);
	struct timespec *uts = (struct timespec *)syscall_arg(tf, 1);
	struct timespec kts;

	if (!clock_id_supported(clock_id))
		return -EINVAL;
	if (!uts)
		return -EFAULT;
	if (clock_id == CLOCK_REALTIME)
		kernel_realtime_now(&kts);
	else
		mtime_to_timespec(timer_now(), &kts);
	if (copy_to_user(uts, &kts, sizeof(kts)) != 0)
		return -EFAULT;
	return 0;
}

ssize_t sys_clock_getres(struct trap_frame *tf)
{
	int clock_id = (int)syscall_arg(tf, 0);
	struct timespec *uts = (struct timespec *)syscall_arg(tf, 1);
	struct timespec kts = {
		.tv_sec = 0,
		.tv_nsec = NSEC_PER_SEC / MTIME_FREQ,
	};

	if (!clock_id_supported(clock_id))
		return -EINVAL;
	if (!uts)
		return 0;
	if (copy_to_user(uts, &kts, sizeof(kts)) != 0)
		return -EFAULT;
	return 0;
}

ssize_t sys_nanosleep(struct trap_frame *tf)
{
	const struct timespec *ureq =
		(const struct timespec *)syscall_arg(tf, 0);
	struct timespec *urem = (struct timespec *)syscall_arg(tf, 1);
	struct wait_deadline deadline;
	struct timespec req;
	wait_outcome_t outcome;
	int ret;

	if (!ureq)
		return -EFAULT;
	if (copy_from_user(&req, ureq, sizeof(req)) != 0)
		return -EFAULT;
	ret = mtime_deadline_from_timespec(&req, &deadline);
	if (ret < 0)
		return ret;

	ret = syscall_wait_deadline(&deadline, &outcome);
	if (ret < 0)
		return ret;
	if (outcome == WAIT_OUTCOME_TIMEOUT)
		return 0;
	if (outcome != WAIT_OUTCOME_SIGNAL)
		return -EINVAL;

	if (urem) {
		struct timespec rem = {0};
		uint64_t after = timer_now();

		if (deadline.expires > after)
			mtime_to_timespec(deadline.expires - after, &rem);
		if (copy_to_user(urem, &rem, sizeof(rem)) != 0)
			return -EFAULT;
	}
	return -EINTR;
}

ssize_t sys_clock_nanosleep(struct trap_frame *tf)
{
	int clock_id = (int)syscall_arg(tf, 0);
	int flags = (int)syscall_arg(tf, 1);
	const struct timespec *ureq =
		(const struct timespec *)syscall_arg(tf, 2);
	struct timespec *urem = (struct timespec *)syscall_arg(tf, 3);
	struct wait_deadline deadline;
	struct timespec req;
	uint64_t value;
	wait_outcome_t outcome;
	int ret;

	if (!clock_id_supported(clock_id))
		return -EINVAL;
	if (flags != 0 && flags != TIMER_ABSTIME)
		return -EINVAL;
	if (clock_id == CLOCK_REALTIME && flags == TIMER_ABSTIME)
		return -EINVAL;
	if (!ureq)
		return -EFAULT;
	if (copy_from_user(&req, ureq, sizeof(req)) != 0)
		return -EFAULT;
	ret = timespec_to_mtime_delta(&req, &value);
	if (ret < 0)
		return ret;

	if (flags == TIMER_ABSTIME)
		deadline = wait_deadline_at(value);
	else {
		ret = mtime_deadline_from_timespec(&req, &deadline);
		if (ret < 0)
			return ret;
	}

	ret = syscall_wait_deadline(&deadline, &outcome);
	if (ret < 0)
		return ret;
	if (outcome == WAIT_OUTCOME_TIMEOUT)
		return 0;
	if (outcome != WAIT_OUTCOME_SIGNAL)
		return -EINVAL;

	if (flags == 0 && urem) {
		struct timespec rem = {0};
		uint64_t after = timer_now();

		if (deadline.expires > after)
			mtime_to_timespec(deadline.expires - after, &rem);
		if (copy_to_user(urem, &rem, sizeof(rem)) != 0)
			return -EFAULT;
	}
	return -EINTR;
}

/* Interval and POSIX timers are intentionally postponed (see SYSCALL.md). */
ssize_t sys_getitimer(struct trap_frame *tf)
{
	(void)tf;
	return -ENOSYS;
}

ssize_t sys_setitimer(struct trap_frame *tf)
{
	(void)tf;
	return -ENOSYS;
}

ssize_t sys_timer_create(struct trap_frame *tf)
{
	(void)tf;
	return -ENOSYS;
}

ssize_t sys_timer_gettime(struct trap_frame *tf)
{
	(void)tf;
	return -ENOSYS;
}

ssize_t sys_timer_getoverrun(struct trap_frame *tf)
{
	(void)tf;
	return -ENOSYS;
}

ssize_t sys_timer_settime(struct trap_frame *tf)
{
	(void)tf;
	return -ENOSYS;
}

ssize_t sys_timer_delete(struct trap_frame *tf)
{
	(void)tf;
	return -ENOSYS;
}

ssize_t sys_clock_settime(struct trap_frame *tf)
{
	int clock_id = (int)syscall_arg(tf, 0);
	const struct timespec *uts =
		(const struct timespec *)syscall_arg(tf, 1);
	struct timespec kts;

	if (clock_id != CLOCK_REALTIME)
		return -EINVAL;
	if (!uts)
		return -EFAULT;
	if (copy_from_user(&kts, uts, sizeof(kts)) != 0)
		return -EFAULT;
	if (task_uid(current_task()) != 0)
		return -EPERM;
	return kernel_realtime_set(&kts);
}
