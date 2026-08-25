/*
 * syscall/sys_sched.c - scheduler-related syscall ABI wrappers
 */

#include <nuvix/errno.h>
#include <nuvix/mm.h>
#include <nuvix/pid.h>
#include <nuvix/sched.h>
#include <nuvix/syscall.h>
#include <nuvix/task.h>
#include <nuvix/trap.h>

/*
 * SYSCALL_SUPPORT(C): sched_setaffinity
 * Current: accepts the configured cpumask width and intersects the requested
 * mask with the scheduler policy. A shorter buffer is rejected.
 * Unsupported errno: empty CPU set returns -EINVAL; missing target returns
 * -ESRCH; unauthorized cross-user target returns -EPERM.
 * Requested and effective masks are retained separately; queued/running
 * updates remain valid under the current CPU0-only policy.
 */
ssize_t sys_sched_setaffinity(struct trap_frame *tf)
{
	long pid = (long)syscall_arg(tf, 0);
	size_t cpusetsize = (size_t)syscall_arg(tf, 1);
	const cpumask_t *umask = (const cpumask_t *)syscall_arg(tf, 2);
	cpumask_t mask;
	struct task_struct *owned __cleanup_with(task_ref) = NULL;
	struct task_struct *task;
	ssize_t ret = 0;

	if (pid < 0 || pid > PID_MAX)
		return -ESRCH;
	if (cpusetsize < cpumask_size())
		return -EINVAL;
	if (!umask)
		return -EFAULT;

	task = task_get_target((pid_t)pid, &owned);
	if (!task)
		return -ESRCH;
	ret = task_access_check(current_task(), task,
				TASK_ACCESS_SCHEDULER_WRITE);
	if (ret < 0)
		goto out;

	/* Larger user buffers are accepted; trailing bytes are ignored. */
	if (copy_from_user(&mask, umask, cpumask_size()) != 0) {
		ret = -EFAULT;
		goto out;
	}
	ret = sched_set_affinity(task, &mask);

out:
	return ret;
}

/*
 * SYSCALL_SUPPORT(C): sched_getaffinity
 * Current: reports the scheduler-owned mask for any existing target task.
 * Unsupported errno: too-small cpusetsize returns -EINVAL; missing target
 * returns -ESRCH.
 * The returned configured-width mask is the effective policy intersection.
 */
ssize_t sys_sched_getaffinity(struct trap_frame *tf)
{
	long pid = (long)syscall_arg(tf, 0);
	size_t cpusetsize = (size_t)syscall_arg(tf, 1);
	cpumask_t *umask = (cpumask_t *)syscall_arg(tf, 2);
	cpumask_t mask;
	struct task_struct *owned __cleanup_with(task_ref) = NULL;
	struct task_struct *task;
	ssize_t ret;

	if (pid < 0 || pid > PID_MAX)
		return -ESRCH;
	if (cpusetsize < cpumask_size())
		return -EINVAL;
	if (!umask)
		return -EFAULT;

	task = task_get_target((pid_t)pid, &owned);
	if (!task)
		return -ESRCH;
	mask = sched_get_affinity(task);

	ret = copy_to_user(umask, &mask, cpumask_size()) != 0
		      ? -EFAULT
		      : (ssize_t)cpumask_size();
	return ret;
}

ssize_t sys_sched_yield(struct trap_frame *tf)
{
	(void)tf;
	sched_yield();
	return 0;
}
