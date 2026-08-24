/*
 * syscall/sys_proc.c - 进程相关系统调用
 */

#include <nuvix/errno.h>
#include <nuvix/syscall.h>
#include <nuvix/exit.h>
#include <nuvix/session.h>
#include <nuvix/task.h>
#include <nuvix/trap.h>

ssize_t sys_getpid(struct trap_frame *tf)
{
	struct task_struct *task = current_task();
	struct proc_struct *proc = task->proc;

	(void)tf;
	return proc && proc->pid ? proc->pid->nr : 0;
}

ssize_t sys_getppid(struct trap_frame *tf)
{
	struct proc_struct *parent;
	pid_t ppid;

	(void)tf;
	if (!current_task()->proc)
		return 0;
	parent = proc_parent_get(current_task()->proc);
	if (!parent)
		return 0;
	ppid = parent->pid ? parent->pid->nr : 0;
	proc_put(parent);
	return ppid;
}

ssize_t sys_getuid(struct trap_frame *tf)
{
	(void)tf;
	return task_uid(current_task());
}

ssize_t sys_geteuid(struct trap_frame *tf)
{
	(void)tf;
	return task_euid(current_task());
}

ssize_t sys_getgid(struct trap_frame *tf)
{
	(void)tf;
	return task_gid(current_task());
}

ssize_t sys_getegid(struct trap_frame *tf)
{
	(void)tf;
	return task_egid(current_task());
}

ssize_t sys_gettid(struct trap_frame *tf)
{
	(void)tf;
	return current_task()->tid ? (ssize_t)current_task()->tid->nr : 0;
}

/*
 * SYSCALL_SUPPORT(B): getpgid
 * Current: returns the process group for self or an existing group leader pid.
 * Unsupported errno: negative or missing pid returns -ESRCH.
 * Future: revisit when sessions and job-control semantics exist.
 */
ssize_t sys_getpgid(struct trap_frame *tf)
{
	long pid = (long)syscall_arg(tf, 0);

	return session_process_getpgid((pid_t)pid);
}

/*
 * SYSCALL_SUPPORT(B): getsid
 * Current: returns the session id for self or an existing group leader pid.
 * Unsupported errno: negative or missing pid returns -ESRCH. Linux does not
 * reject cross-session getsid() with EPERM, and neither does nuvix.
 * Future: revisit permission checks when a fuller credential model exists.
 */
ssize_t sys_getsid(struct trap_frame *tf)
{
	long pid = (long)syscall_arg(tf, 0);

	return session_process_getsid((pid_t)pid);
}

/*
 * SYSCALL_SUPPORT(B): setsid
 * Current: creates a new session and process group for the current process
 * when no process group already uses the caller's process id.
 * Unsupported errno: process-group leaders return -EPERM.
 * Future: connect multiple controlling terminals if nuvix grows more tty
 * devices.
 */
ssize_t sys_setsid(struct trap_frame *tf)
{
	(void)tf;
	return session_process_setsid(current_task());
}

/*
 * SYSCALL_SUPPORT(B): setpgid
 * Current: updates self or a direct child into an existing/new pgid within the
 * same session; session leaders cannot change process group.
 * Unsupported errno: negative pid/pgid returns -EINVAL; missing target returns
 * -ESRCH; non-child targets, cross-session moves, missing target groups, and
 * session leaders return -EPERM.
 * Future: model exec-time EACCES and orphaned process-group rules.
 */
ssize_t sys_setpgid(struct trap_frame *tf)
{
	long pid = (long)syscall_arg(tf, 0);
	long pgid = (long)syscall_arg(tf, 1);

	if (pid < 0 || pgid < 0)
		return -EINVAL;

	return session_process_setpgid((pid_t)pid, (pid_t)pgid);
}

ssize_t sys_exit(struct trap_frame *tf)
{
	int code = (int)syscall_arg(tf, 0);
	do_exit(code);
	unreachable();
}

ssize_t sys_exit_group(struct trap_frame *tf)
{
	int code = (int)syscall_arg(tf, 0);
	do_exit_group(code);
	unreachable();
}
