/*
 * syscall/sys_task.c - task lifecycle syscall ABI wrappers
 */

#include <kernel/errno.h>
#include <kernel/exit.h>
#include <kernel/fork.h>
#include <kernel/mm.h>
#include <kernel/resource.h>
#include <kernel/signal.h>
#include <kernel/syscall.h>
#include <uapi/sched.h>
#include <kernel/trap.h>

static int sys_write_tid(int *uaddr, pid_t tid)
{
	if (!uaddr)
		return -EFAULT;
	if (copy_to_user(uaddr, &tid, sizeof(tid)) != 0)
		return -EFAULT;
	return 0;
}

/*
 * SYSCALL_SUPPORT(B): clone
 * Current: supports fork-like clone, a thread subset, and non-thread vfork
 * through kernel_clone; see SYSCALL.md for the clone flag support table.
 * Unsupported errno: namespace, parent, io, CLONE_VFORK | CLONE_THREAD, and
 * invalid flag combinations return -EINVAL.
 * Future: extend only when a concrete runtime needs a rejected flag.
 */
ssize_t sys_clone(struct trap_frame *tf)
{
	unsigned long flags = (unsigned long)syscall_arg(tf, 0);
	uintptr_t child_stack = (uintptr_t)syscall_arg(tf, 1);
	int *parent_tid = (int *)syscall_arg(tf, 2);
	uintptr_t tls = (uintptr_t)syscall_arg(tf, 3);
	int *child_tid = (int *)syscall_arg(tf, 4);
	struct kernel_clone clone;
	int ret;

	ret = kernel_clone_prepare(tf, flags, child_stack, tls, child_tid,
				   &clone);
	if (ret < 0)
		return ret;

	if (flags & CLONE_PARENT_SETTID) {
		ret = sys_write_tid(parent_tid, clone.pid);
		if (ret < 0)
			goto abort;
	}
	return kernel_clone_commit(&clone);

abort:
	kernel_clone_abort(&clone);
	return ret;
}

/*
 * SYSCALL_SUPPORT(B): wait4
 * Current: supports all pid selectors, WNOHANG/WUNTRACED/WCONTINUED and the
 * Linux internal __WNOTHREAD/__WCLONE/__WALL filters. It can return rusage;
 * SA_RESTART replays an interrupted blocking wait after its handler returns.
 * Unsupported errno: unknown option bits return -EINVAL; no wait target
 * returns -ECHILD.
 */
ssize_t sys_wait4(struct trap_frame *tf)
{
	long pid = (long)syscall_arg(tf, 0);
	int *wstatus = (int *)syscall_arg(tf, 1);
	int options = (int)syscall_arg(tf, 2);
	struct rusage *urusage = (struct rusage *)syscall_arg(tf, 3);
	struct wait4_result result = {0};
	int ret;

	ret = kernel_wait4(pid, options, &result);
	if (ret < 0)
		return ret;
	if (result.pid == 0)
		return 0;
	if (wstatus && !access_ok(wstatus, sizeof(*wstatus)))
		goto fault;
	if (urusage && !access_ok(urusage, sizeof(*urusage)))
		goto fault;

	if (wstatus &&
	    copy_to_user(wstatus, &result.status, sizeof(result.status)) != 0)
		goto fault;

	if (urusage) {
		struct rusage rusage;

		cputime_rusage(&result.cputime, &rusage);
		if (copy_to_user(urusage, &rusage, sizeof(rusage)) != 0)
			goto fault;
	}

	pid_t waited_pid = result.pid;
	kernel_wait4_finish(&result);
	return waited_pid;

fault:
	kernel_wait4_abort(&result);
	return -EFAULT;
}
