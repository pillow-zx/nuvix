/*
 * syscall/sys_futex.c - futex and robust-list syscall ABI wrappers
 */

#include <nuvix/errno.h>
#include <nuvix/futex.h>
#include <nuvix/mm.h>
#include <nuvix/syscall.h>
#include <nuvix/task.h>
#include <nuvix/time.h>
#include <nuvix/trap.h>
#include <nuvix/wait.h>

static int futex_copy_deadline(const struct timespec *utimeout,
				       struct wait_deadline *deadline, bool absolute)
{
	struct timespec timeout;
	uint64_t expires;
	int ret;

	if (!deadline)
		return -EINVAL;

	*deadline = wait_deadline_none();
	if (!utimeout)
		return 0;

	if (copy_from_user(&timeout, utimeout, sizeof(timeout)) != 0)
		return -EFAULT;

	if (!absolute)
		return mtime_deadline_from_timespec(&timeout, deadline);
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
	struct futex_args args;
	int cmd = op & FUTEX_CMD_MASK;
	int ret;

	if (cmd == FUTEX_WAIT) {
		ret = futex_copy_deadline(timeout, &deadline, false);
		if (ret < 0)
			return ret;
	} else if (cmd == FUTEX_WAIT_BITSET) {
		ret = futex_copy_deadline(timeout, &deadline, true);
		if (ret < 0)
			return ret;
	}

	args.uaddr = uaddr;
	args.op = op;
	args.val = val;
	args.deadline = &deadline;
	args.uaddr2 = uaddr2;
	args.val3 = val3;
	return futex(&args);
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
