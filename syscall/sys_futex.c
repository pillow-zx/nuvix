/*
 * syscall/sys_futex.c - futex and robust-list syscall ABI wrappers
 */

#include <nuvix/errno.h>
#include <nuvix/futex.h>
#include <nuvix/mm.h>
#include <nuvix/pid.h>
#include <nuvix/syscall.h>
#include <nuvix/task.h>
#include <nuvix/time.h>
#include <nuvix/trap.h>
#include <nuvix/wait.h>

static int futex_copy_timeout(const struct timespec *utimeout,
			      struct wait_deadline *deadline)
{
	struct timespec timeout;
	int ret;

	if (!deadline)
		return -EINVAL;

	deadline->active = false;
	deadline->expires = 0;
	if (!utimeout)
		return 0;

	if (copy_from_user(&timeout, utimeout, sizeof(timeout)) != 0)
		return -EFAULT;

	ret = mtime_deadline_from_timespec(&timeout, deadline);
	return ret;
}

static int futex_copy_absolute_timeout(const struct timespec *utimeout,
				       struct wait_deadline *deadline)
{
	struct timespec timeout;
	uint64_t expires;
	int ret;

	if (!deadline)
		return -EINVAL;

	deadline->active = false;
	deadline->expires = 0;
	if (!utimeout)
		return 0;

	if (copy_from_user(&timeout, utimeout, sizeof(timeout)) != 0)
		return -EFAULT;

	ret = timespec_to_mtime_delta(&timeout, &expires);
	if (ret < 0)
		return ret;

	deadline->active = true;
	deadline->expires = expires;
	return 0;
}

/*
 * SYSCALL_SUPPORT(B): futex
 * Current: supports FUTEX_WAIT/FUTEX_WAKE, WAIT_BITSET/WAKE_BITSET, PRIVATE
 * aliases, and robust-list exit wakeups.
 * A timeout-free FUTEX_WAIT is replayed after an SA_RESTART handler returns.
 * Unsupported errno: FUTEX_CLOCK_REALTIME, requeue, PI, and unknown ops
 * return -ENOSYS from kernel_futex().
 * Future: add requeue and PI ops only by pthread/libc demand.
 */
ssize_t sys_futex(struct trap_frame *tf)
{
	int *uaddr = (int *)syscall_arg(tf, 0);
	int op = (int)syscall_arg(tf, 1);
	int val = (int)syscall_arg(tf, 2);
	const struct timespec *timeout =
		(const struct timespec *)syscall_arg(tf, 3);
	int *uaddr2 = (int *)syscall_arg(tf, 4);
	int val3 = (int)syscall_arg(tf, 5);
	struct wait_deadline deadline;
	struct kernel_futex_args args;
	int cmd = op & FUTEX_CMD_MASK;
	int ret;

	deadline.active = false;
	deadline.expires = 0;
	if (cmd == FUTEX_WAIT) {
		ret = futex_copy_timeout(timeout, &deadline);
		if (ret < 0)
			return ret;
	} else if (cmd == FUTEX_WAIT_BITSET) {
		ret = futex_copy_absolute_timeout(timeout, &deadline);
		if (ret < 0)
			return ret;
	}

	args.uaddr = uaddr;
	args.op = op;
	args.val = val;
	args.deadline = &deadline;
	args.uaddr2 = uaddr2;
	args.val3 = val3;
	return kernel_futex(&args);
}

/*
 * SYSCALL_SUPPORT(B): set_robust_list
 * Current: records the current task robust-list head for exit-time walking.
 * Unsupported errno: len different from struct robust_list_head returns
 * -EINVAL.
 * Future: keep this stable while adding invalid-chain stress coverage.
 */
ssize_t sys_set_robust_list(struct trap_frame *tf)
{
	struct robust_list_head *head = (struct robust_list_head *)syscall_arg(tf, 0);
	size_t len = (size_t)syscall_arg(tf, 1);

	return futex_set_robust_list(current_task(), head, len);
}

/*
 * SYSCALL_SUPPORT(B): get_robust_list
 * Current: queries pid 0 or an existing thread's robust-list pointer and len.
 * Unsupported errno: negative pid returns -EINVAL; missing task returns -ESRCH;
 * cross-thread permission checks are shallow.
 * Future: add permission behavior when credentials are deepened.
 */
ssize_t sys_get_robust_list(struct trap_frame *tf)
{
	long pid = (long)syscall_arg(tf, 0);
	struct robust_list_head **uhead = (struct robust_list_head **)syscall_arg(tf, 1);
	size_t *ulen = (size_t *)syscall_arg(tf, 2);
	struct task_struct *task;
	struct robust_list_head *head;
	size_t len;
	int ret;
	bool put_task = false;

	if (!uhead || !ulen)
		return -EFAULT;
	if (pid < 0)
		return -EINVAL;

	if (pid == 0) {
		task = current_task();
	} else {
		task = pid_lookup_task((pid_t)pid);
		put_task = true;
	}
	if (!task)
		return -ESRCH;

	ret = futex_get_robust_list(task, &head, &len);
	if (ret < 0) {
		if (put_task)
			task_put(task);
		return ret;
	}
	if (copy_to_user(uhead, &head, sizeof(head)) != 0) {
		if (put_task)
			task_put(task);
		return -EFAULT;
	}
	if (copy_to_user(ulen, &len, sizeof(len)) != 0) {
		if (put_task)
			task_put(task);
		return -EFAULT;
	}
	if (put_task)
		task_put(task);

	return 0;
}
