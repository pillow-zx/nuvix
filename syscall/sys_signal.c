/*
 * syscall/sys_signal.c - 信号相关系统调用（ABI 边界层）
 */

#include <nuvix/errno.h>
#include <nuvix/mm.h>
#include <nuvix/signal.h>
#include <nuvix/syscall.h>
#include <nuvix/types.h>
#include <nuvix/trap.h>

/*
 * SYSCALL_SUPPORT(B): kill
 * Current: delivers to a positive pid, the caller's process group for pid 0,
 * a selected process group for pid < -1, or all eligible processes for -1.
 * Unsupported errno: a missing target set returns -ESRCH.
 * Future: add credential-based permission semantics.
 */
ssize_t sys_kill(struct trap_frame *tf)
{
	long pid = (long)syscall_arg(tf, 0);
	int sig = (int)syscall_arg(tf, 1);

	return do_kill(pid, sig);
}

/*
 * SYSCALL_SUPPORT(B): tkill
 * Current: delivers to a positive tid.
 * Unsupported errno: tid <= 0 returns -EINVAL; missing target returns -ESRCH.
 * Future: add credential checks with the permission model.
 */
ssize_t sys_tkill(struct trap_frame *tf)
{
	long tid = (long)syscall_arg(tf, 0);
	int sig = (int)syscall_arg(tf, 1);

	if (tid <= 0)
		return -EINVAL;

	return do_tkill(tid, sig);
}

/*
 * SYSCALL_SUPPORT(B): tgkill
 * Current: delivers to a positive tgid/tid pair.
 * Unsupported errno: non-positive ids return -EINVAL; missing or mismatched
 * target returns -ESRCH.
 * Future: add credential checks with tkill.
 */
ssize_t sys_tgkill(struct trap_frame *tf)
{
	long tgid = (long)syscall_arg(tf, 0);
	long tid = (long)syscall_arg(tf, 1);
	int sig = (int)syscall_arg(tf, 2);

	if (tgid <= 0 || tid <= 0)
		return -EINVAL;

	return do_tgkill(tgid, tid, sig);
}

/*
 * SYSCALL_SUPPORT(B): sigaltstack
 * Current: registers, disables, or queries one alternate signal stack.
 * Unsupported errno: changing while on-stack returns -EPERM; unknown ss_flags
 * return -EINVAL; too-small stacks return -ENOMEM.
 * Future: document flag policy such as SS_AUTODISARM.
 */
ssize_t sys_sigaltstack(struct trap_frame *tf)
{
	const struct stack_t *ss = (const struct stack_t *)syscall_arg(tf, 0);
	struct stack_t *old_ss = (struct stack_t *)syscall_arg(tf, 1);
	struct stack_t kss;
	struct stack_t old;
	int ret;

	if (old_ss) {
		ret = do_sigaltstack(NULL, &old);
		if (ret < 0)
			return ret;
		if (copy_to_user(old_ss, &old, sizeof(old)) != 0)
			return -EFAULT;
	}

	if (!ss)
		return 0;

	if (copy_from_user(&kss, ss, sizeof(kss)) != 0)
		return -EFAULT;
	return do_sigaltstack(&kss, NULL);
}

/*
 * SYSCALL_SUPPORT(B): rt_sigaction
 * Current: installs/query handlers and masks with a fixed unsigned long sigset.
 * Unsupported errno: bad sigset size, uncatchable signals with act, or SIG_ERR
 * handler returns -EINVAL; SA_* semantics are shallow.
 * Future: build a signal action flag matrix.
 */
ssize_t sys_sigaction(struct trap_frame *tf)
{
	int sig = (int)syscall_arg(tf, 0);
	const struct sigaction *act =
		(const struct sigaction *)syscall_arg(tf, 1);
	struct sigaction *oldact = (struct sigaction *)syscall_arg(tf, 2);
	size_t sigsetsize = (size_t)syscall_arg(tf, 3);
	struct sigaction kact;
	struct sigaction old;
	int ret;

	if (!signal_is_valid(sig))
		return -EINVAL;
	if (!signal_is_catchable(sig) && act)
		return -EINVAL;
	if (sigsetsize != sizeof(unsigned long))
		return -EINVAL;

	if (oldact) {
		ret = do_sigaction(sig, NULL, &old);
		if (ret < 0)
			return ret;
		if (copy_to_user(oldact, &old, sizeof(old)) != 0)
			return -EFAULT;
	}

	if (!act)
		return 0;

	if (copy_from_user(&kact, act, sizeof(kact)) != 0)
		return -EFAULT;
	return do_sigaction(sig, &kact, NULL);
}

/*
 * SYSCALL_SUPPORT(B): rt_sigprocmask
 * Current: updates/query blocked mask with an unsigned long sigset ABI.
 * Unsupported errno: bad sigset size or invalid how value returns -EINVAL.
 * Future: keep ABI size assertions and expand signal-mask edge coverage.
 */
ssize_t sys_sigprocmask(struct trap_frame *tf)
{
	int how = (int)syscall_arg(tf, 0);
	const uint64_t *set = (const uint64_t *)syscall_arg(tf, 1);
	uint64_t *oldset = (uint64_t *)syscall_arg(tf, 2);
	size_t sigsetsize = (size_t)syscall_arg(tf, 3);
	uint64_t newset;
	uint64_t old;
	int ret;

	if (sigsetsize != sizeof(unsigned long))
		return -EINVAL;

	if (oldset) {
		ret = do_sigprocmask(how, NULL, &old);
		if (ret < 0)
			return ret;
		if (copy_to_user(oldset, &old, sizeof(old)) != 0)
			return -EFAULT;
	}

	if (!set)
		return 0;

	if (copy_from_user(&newset, set, sizeof(newset)) != 0)
		return -EFAULT;
	return do_sigprocmask(how, &newset, NULL);
}

/*
 * SYSCALL_SUPPORT(B): rt_sigpending
 * Current: snapshots the calling thread's private and thread-group pending
 * masks using the Linux riscv64 unsigned-long sigset ABI.
 * Unsupported errno: a bad sigset size returns -EINVAL; an invalid userspace
 * destination returns -EFAULT.
 */
ssize_t sys_sigpending(struct trap_frame *tf)
{
	uint64_t *set = (uint64_t *)syscall_arg(tf, 0);
	size_t sigsetsize = (size_t)syscall_arg(tf, 1);
	uint64_t pending;

	if (sigsetsize != sizeof(unsigned long))
		return -EINVAL;
	pending = signal_pending_mask(current_task());
	if (copy_to_user(set, &pending, sizeof(pending)) != 0)
		return -EFAULT;
	return 0;
}

/*
 * SYSCALL_SUPPORT(B): rt_sigsuspend
 * Current: replaces the calling mask while waiting for an unblocked signal,
 * then returns -EINTR and restores the prior mask after handler return.
 * Unsupported: real-time signals 32..64 and queued duplicate instances.
 * Future: extend the signal model only when a real workload needs RT signals.
 */
ssize_t sys_sigsuspend(struct trap_frame *tf)
{
	const uint64_t *uset = (const uint64_t *)syscall_arg(tf, 0);
	size_t sigsetsize = (size_t)syscall_arg(tf, 1);
	uint64_t set;

	if (sigsetsize != sizeof(unsigned long))
		return -EINVAL;
	if (copy_from_user(&set, uset, sizeof(set)) != 0)
		return -EFAULT;
	return signal_sigsuspend(set);
}

/*
 * SYSCALL_SUPPORT(B): rt_sigtimedwait
 * Current: consumes thread-private or shared standard signals, waits with an
 * optional relative timeout, returns siginfo, and is interrupted by an
 * unrelated deliverable signal.
 * Unsupported: real-time signals 32..64 and queued duplicate instances.
 * Future: extend the signal model only when a real workload needs RT signals.
 */
ssize_t sys_sigtimedwait(struct trap_frame *tf)
{
	const uint64_t *uset = (const uint64_t *)syscall_arg(tf, 0);
	siginfo_t *uinfo = (siginfo_t *)syscall_arg(tf, 1);
	const struct timespec *utimeout =
		(const struct timespec *)syscall_arg(tf, 2);
	size_t sigsetsize = (size_t)syscall_arg(tf, 3);
	struct timespec timeout;
	siginfo_t info;
	uint64_t set;
	int ret;

	if (sigsetsize != sizeof(uint64_t))
		return -EINVAL;
	if (copy_from_user(&set, uset, sizeof(set)) != 0)
		return -EFAULT;
	if (utimeout &&
	    copy_from_user(&timeout, utimeout, sizeof(timeout)) != 0)
		return -EFAULT;

	ret = signal_wait_pending(set, utimeout ? &timeout : NULL, &info);
	if (ret > 0 && uinfo && copy_to_user(uinfo, &info, sizeof(info)) != 0)
		return -EFAULT;
	return ret;
}

/*
 * SYSCALL_SUPPORT(B): rt_sigreturn
 * Current: restores the current kernel-tracked user signal frame. Nested
 * handlers return in LIFO order; unmatched or invalid frames terminate with
 * SIGSEGV.
 * Future: continue architecture-state and restart-safety coverage.
 */
ssize_t sys_sigreturn(struct trap_frame *tf)
{
	uintptr_t sp = trap_user_sp(tf);

	return do_sigreturn(tf, sp);
}
