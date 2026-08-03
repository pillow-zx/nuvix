/*
 * syscall/sys_time.c - time syscall ABI wrappers
 *
 * The timer_* entry points below are implemented as a small POSIX timer
 * subset.  Keep support boundaries here and in SYSCALL.md aligned; these
 * handlers are not blanket -ENOSYS stubs.
 */

#include <kernel/errno.h>
#include <kernel/mm.h>
#include <kernel/signal.h>
#include <kernel/syscall.h>
#include <kernel/task.h>
#include <kernel/time.h>
#include <kernel/timer.h>
#include <kernel/types.h>
#include <kernel/trap.h>
#include <kernel/wait.h>

/*
 * SYSCALL_SUPPORT(B): times
 * Current: reports current task user/system ticks and current jiffies.
 * Unsupported errno: child CPU time remains partial and zero when unavailable.
 * Future: add fuller child cputime accounting coverage.
 */
ssize_t sys_times(struct trap_frame *tf)
{
	struct tms *utms = (struct tms *)syscall_arg(tf, 0);
	struct tms ktms = {
		.tms_utime = (int64_t)task_user_ticks(current_task()),
		.tms_stime = (int64_t)task_system_ticks(current_task()),
		.tms_cutime = 0,
		.tms_cstime = 0,
	};

	if (utms && copy_to_user(utms, &ktms, sizeof(ktms)) != 0)
		return -EFAULT;

	return (ssize_t)jiffies;
}

/*
 * SYSCALL_SUPPORT(B): gettimeofday
 * Current: reports the runtime CLOCK_REALTIME wall-clock offset as timeval and
 * UTC timezone zeroes. The offset is volatile because no RTC exists.
 * Unsupported errno: timezone policy is fixed to UTC zeroes.
 * Future: add an RTC source and persistence policy when the device exists.
 */
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

/*
 * SYSCALL_SUPPORT(B): clock_gettime
 * Current: REALTIME is mtime plus a volatile wall-clock offset; MONOTONIC and
 * BOOTTIME remain raw mtime.
 * Unsupported errno: unsupported clock ids return -EINVAL.
 * Future: add an RTC source and persistence policy when the device exists.
 */
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
		.tv_nsec = 1000000000UL / MTIME_FREQ,
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

	ret = wait_for_interruptible(NULL, &deadline, &outcome);
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

/*
 * SYSCALL_SUPPORT(B): clock_nanosleep
 * Current: supports relative sleep and TIMER_ABSTIME for supported clocks.
 * Unsupported errno: unsupported clocks or flags return -EINVAL; clock
 * differences are shallow because supported clocks share mtime.
 * Future: align this with clock_gettime when clock sources diverge.
 */
ssize_t sys_clock_nanosleep(struct trap_frame *tf)
{
	int clock_id = (int)syscall_arg(tf, 0);
	int flags = (int)syscall_arg(tf, 1);
	const struct timespec *ureq =
		(const struct timespec *)syscall_arg(tf, 2);
	struct timespec *urem = (struct timespec *)syscall_arg(tf, 3);
	struct wait_deadline deadline;
	struct timespec req;
	uint64_t delta;
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

	ret = timespec_to_mtime_delta(&req, &delta);
	if (ret < 0)
		return ret;

	if (flags == TIMER_ABSTIME)
		deadline = wait_deadline_at(delta);
	else if (flags == 0) {
		ret = mtime_deadline_from_timespec(&req, &deadline);
		if (ret < 0)
			return ret;
	}

	ret = wait_for_interruptible(NULL, &deadline, &outcome);
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

/*
 * SYSCALL_SUPPORT(B): getitimer
 * Current: queries per-signal itimer state, with ITIMER_REAL as the useful
 * path.
 * Unsupported errno: invalid timer kind returns -EINVAL.
 * Future: document ITIMER_REAL priority until virtual/prof accounting exists.
 */
ssize_t sys_getitimer(struct trap_frame *tf)
{
	int which = (int)syscall_arg(tf, 0);
	struct itimerval *uvalue = (struct itimerval *)syscall_arg(tf, 1);
	struct signal_struct *signal;
	struct itimerval value;
	int ret;

	if (!itimer_which_valid(which))
		return -EINVAL;
	if (!uvalue)
		return -EFAULT;

	signal = task_signal_state(current_task());
	if (!signal)
		return -EINVAL;

	ret = itimer_get_value(&signal->itimers[itimer_which_index(which)],
			       &value);
	if (ret < 0)
		return ret;

	if (copy_to_user(uvalue, &value, sizeof(value)) != 0)
		return -EFAULT;

	return 0;
}

/*
 * SYSCALL_SUPPORT(B): setitimer
 * Current: supports ITIMER_REAL through signal timer state.
 * Unsupported errno: ITIMER_VIRTUAL and ITIMER_PROF return -EINVAL.
 * Future: add CPU accounting before expanding virtual/prof timers.
 */
ssize_t sys_setitimer(struct trap_frame *tf)
{
	int which = (int)syscall_arg(tf, 0);
	const struct itimerval *unew_value =
		(const struct itimerval *)syscall_arg(tf, 1);
	struct itimerval *uold_value = (struct itimerval *)syscall_arg(tf, 2);
	struct itimerval new_value = {0};
	struct itimerval old_value;
	struct signal_struct *signal;
	struct task_struct *target;
	int ret;

	if (!itimer_which_valid(which))
		return -EINVAL;
	if (which != ITIMER_REAL)
		return -EINVAL;

	if (unew_value &&
	    copy_from_user(&new_value, unew_value, sizeof(new_value)) != 0)
		return -EFAULT;

	signal = task_signal_state(current_task());
	if (!signal)
		return -EINVAL;

	target = task_group_leader_safe(current_task());
	if (!target)
		target = current_task();
	ret = itimer_set_real(&signal->itimers[ITIMER_REAL], target, &new_value,
			      uold_value ? &old_value : NULL);
	if (ret < 0)
		return ret;

	if (uold_value &&
	    copy_to_user(uold_value, &old_value, sizeof(old_value)) != 0)
		return -EFAULT;

	return 0;
}

/*
 * SYSCALL_SUPPORT(B): timer_create
 * Current: creates per-signal POSIX timers for supported clocks and sigevents.
 * Unsupported errno: unsupported clock or sigevent policy is returned by
 * posix_timer_create(), commonly -EINVAL.
 * Future: add a sigevent and clock support table.
 */
ssize_t sys_timer_create(struct trap_frame *tf)
{
	clockid_t clock_id = (clockid_t)syscall_arg(tf, 0);
	const sigevent_t *usevp = (const sigevent_t *)syscall_arg(tf, 1);
	timer_t *utimerid = (timer_t *)syscall_arg(tf, 2);
	sigevent_t event;
	const sigevent_t *eventp = NULL;
	struct signal_struct *signal;
	struct task_struct *target;
	timer_t timerid;
	int ret;

	if (!utimerid)
		return -EFAULT;
	if (usevp) {
		if (copy_from_user(&event, usevp, sizeof(event)) != 0)
			return -EFAULT;
		eventp = &event;
	}

	signal = task_signal_state(current_task());
	if (!signal)
		return -EINVAL;

	target = task_group_leader_safe(current_task());
	if (!target)
		target = current_task();

	ret = posix_timer_create(signal, clock_id, &timerid, eventp, target);
	if (ret < 0)
		return ret;

	if (copy_to_user(utimerid, &timerid, sizeof(timerid)) != 0) {
		int delete_ret = posix_timer_delete(signal, timerid);

		(void)delete_ret;
		return -EFAULT;
	}
	return 0;
}

/*
 * SYSCALL_SUPPORT(B): timer_gettime
 * Current: queries a timer owned by the current signal state.
 * Unsupported errno: invalid timer ids return timer-layer errno.
 * Future: keep aligned with POSIX timer lifecycle coverage.
 */
ssize_t sys_timer_gettime(struct trap_frame *tf)
{
	timer_t timerid = (timer_t)syscall_arg(tf, 0);
	struct itimerspec *uvalue = (struct itimerspec *)syscall_arg(tf, 1);
	struct signal_struct *signal;
	struct itimerspec value;
	int ret;

	if (!uvalue)
		return -EFAULT;

	signal = task_signal_state(current_task());
	if (!signal)
		return -EINVAL;

	ret = posix_timer_gettime(signal, timerid, &value);
	if (ret < 0)
		return ret;

	if (copy_to_user(uvalue, &value, sizeof(value)) != 0)
		return -EFAULT;
	return 0;
}

/*
 * SYSCALL_SUPPORT(B): timer_getoverrun
 * Current: returns the timer overrun count from current signal timer state.
 * Unsupported errno: invalid timer ids return timer-layer errno.
 * Future: deepen when signal queue semantics are expanded.
 */
ssize_t sys_timer_getoverrun(struct trap_frame *tf)
{
	timer_t timerid = (timer_t)syscall_arg(tf, 0);
	struct signal_struct *signal;

	signal = task_signal_state(current_task());
	if (!signal)
		return -EINVAL;
	return posix_timer_getoverrun(signal, timerid);
}

/*
 * SYSCALL_SUPPORT(B): timer_settime
 * Current: arms/disarms timers with relative or absolute expiry.
 * Unsupported errno: invalid flags, ids, or timespecs return timer-layer errno.
 * Future: add coverage for realtime offset and flag combinations.
 */
ssize_t sys_timer_settime(struct trap_frame *tf)
{
	timer_t timerid = (timer_t)syscall_arg(tf, 0);
	int flags = (int)syscall_arg(tf, 1);
	const struct itimerspec *unew_value =
		(const struct itimerspec *)syscall_arg(tf, 2);
	struct itimerspec *uold_value = (struct itimerspec *)syscall_arg(tf, 3);
	struct itimerspec new_value;
	struct itimerspec old_value;
	struct signal_struct *signal;
	int ret;

	if (!unew_value)
		return -EFAULT;
	if (copy_from_user(&new_value, unew_value, sizeof(new_value)) != 0)
		return -EFAULT;

	signal = task_signal_state(current_task());
	if (!signal)
		return -EINVAL;

	ret = posix_timer_settime(signal, timerid, flags, &new_value,
				  uold_value ? &old_value : NULL);
	if (ret < 0)
		return ret;

	if (uold_value &&
	    copy_to_user(uold_value, &old_value, sizeof(old_value)) != 0)
		return -EFAULT;
	return 0;
}

/*
 * SYSCALL_SUPPORT(B): timer_delete
 * Current: deletes timers from the current signal timer state.
 * Unsupported errno: invalid timer ids return timer-layer errno.
 * Future: keep lifecycle behavior aligned with timer_create/settime tests.
 */
ssize_t sys_timer_delete(struct trap_frame *tf)
{
	timer_t timerid = (timer_t)syscall_arg(tf, 0);
	struct signal_struct *signal;

	signal = task_signal_state(current_task());
	if (!signal)
		return -EINVAL;
	return posix_timer_delete(signal, timerid);
}

/*
 * SYSCALL_SUPPORT(B): clock_settime
 * Current: root may set CLOCK_REALTIME through the volatile wall-clock offset.
 * Unsupported errno: non-root callers return -EPERM; unsupported clock ids,
 * malformed timespecs, and values before CLOCK_MONOTONIC return -EINVAL.
 * Future: add an RTC source and persistence policy when the device exists.
 */
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
