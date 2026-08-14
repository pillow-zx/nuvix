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

static struct task_struct *affinity_target_task(pid_t pid, bool *put_task)
{
	if (pid == 0) {
		*put_task = false;
		return current_task();
	}

	*put_task = true;
	return pid_lookup_task(pid);
}

/*
 * SYSCALL_SUPPORT(C): sched_setaffinity
	 * Current: stores the requested mask intersected with online CPUs.
 * Unsupported errno: empty CPU set returns -EINVAL; missing target returns
 * -ESRCH; unauthorized cross-user target returns -EPERM.
	 * The scheduler owns the stored affinity and rejects changes while queued or
	 * running; the current single-CPU adapter therefore accepts only bit 0.
 */
ssize_t sys_sched_setaffinity(struct trap_frame *tf)
{
	long pid = (long)syscall_arg(tf, 0);
	size_t cpusetsize = (size_t)syscall_arg(tf, 1);
	const unsigned long *umask = (const unsigned long *)syscall_arg(tf, 2);
	unsigned long mask = 0;
	size_t copy_size;
	struct task_struct *task;
	bool put_task;
	ssize_t ret = 0;

	if (pid < 0)
		return -ESRCH;
	if (cpusetsize == 0)
		return -EINVAL;
	if (!umask)
		return -EFAULT;

	task = affinity_target_task(pid, &put_task);
	if (!task)
		return -ESRCH;
	if (task != current_task() && current_task() &&
	    task_uid(current_task()) != 0 &&
	    task_uid(current_task()) != task_uid(task)) {
		ret = -EPERM;
		goto out;
	}

	copy_size = cpusetsize < sizeof(mask) ? cpusetsize : sizeof(mask);
	if (copy_from_user(&mask, umask, copy_size) != 0) {
		ret = -EFAULT;
		goto out;
	}
	ret = sched_set_affinity(task, mask);

out:
	if (put_task)
		task_put(task);
	return ret;
}

/*
 * SYSCALL_SUPPORT(C): sched_getaffinity
	 * Current: reports the scheduler-owned mask for any existing target task.
 * Unsupported errno: too-small cpusetsize returns -EINVAL; missing target
 * returns -ESRCH.
	 * The returned mask is intersected with the currently online CPUs.
 */
ssize_t sys_sched_getaffinity(struct trap_frame *tf)
{
	long pid = (long)syscall_arg(tf, 0);
	size_t cpusetsize = (size_t)syscall_arg(tf, 1);
	unsigned long *umask = (unsigned long *)syscall_arg(tf, 2);
	unsigned long mask;
	struct task_struct *task;
	bool put_task;
	ssize_t ret;

	if (pid < 0)
		return -ESRCH;
	if (cpusetsize < sizeof(mask))
		return -EINVAL;
	if (!umask)
		return -EFAULT;

	task = affinity_target_task(pid, &put_task);
	if (!task)
		return -ESRCH;
	mask = (unsigned long)sched_get_affinity(task);

	ret = copy_to_user(umask, &mask, sizeof(mask)) != 0
		      ? -EFAULT
		      : (ssize_t)sizeof(mask);
	if (put_task)
		task_put(task);

	return ret;
}

ssize_t sys_sched_yield(struct trap_frame *tf)
{
	(void)tf;
	sched_yield();
	return 0;
}
