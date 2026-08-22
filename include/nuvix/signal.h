#ifndef _NUVIX_SIGNAL_H
#define _NUVIX_SIGNAL_H

/**
 * @file signal.h
 * @brief Cross-subsystem signal contracts.
 */

#include <nuvix/compiler.h>
#include <nuvix/types.h>
#include <uapi/signal.h>

struct trap_frame;
struct task_struct;
struct proc_struct;
struct pgrp_struct;
struct session_struct;
struct sighand_struct;
struct task_wait;
struct timespec;
struct proc_orphan_event;
struct proc_parent_event;

/**
 * @struct sigchld_exit_policy
 * @brief SIGCHLD exit handling decisions from one action snapshot.
 */
struct sigchld_exit_policy {
	bool auto_reap;
	bool notify;
};

__must_check __const
bool sig_valid(int sig);

__must_check __const
bool sig_catchable(int sig);

__must_check __access_no_size(read_only, 1)
uint64_t sig_blocked_mask(struct task_struct *task);

__access_no_size(read_write, 1)
void sig_set_mask(struct task_struct *task, uint64_t mask);

/** Return the pending signals that are currently blocked for @p task. */
__must_check __access_no_size(read_only, 1)
uint64_t sig_pending(const struct task_struct *task);

/**
 * @brief Restore a temporary wait mask after the next user signal delivery.
 * @param task Task returning from an interruptible masked wait.
 * @param mask Signal mask active before the temporary wait mask.
 */
__access_no_size(read_write, 1)
void sig_defer_mask_restore(struct task_struct *task, uint64_t mask);

/**
 * @brief Deliver one signal to every task of an already-snapshot pgrp.
 * @param sig Signal number, or 0 for an existence probe.
 * @param info Signal info; must be non-NULL when @p sig is non-zero.
 * @param pgrp Snapshot pgrp reference held by the caller.
 * @param session Snapshot session reference holding @p pgrp.
 * @return 0 if delivered to at least one task, -ESRCH for an empty or
 *         mismatched pgrp, or a negative errno from delivery.
 *
 * The caller must hold strong references to @p pgrp and @p session.  IDs are
 * never re-resolved from their numeric values inside or after this call.
 */
__access_no_size(read_only, 2)
__access_no_size(read_only, 3) __access_no_size(read_only, 4)
int sig_send_pgrp(int sig, const siginfo_t *info, struct pgrp_struct *pgrp,
		struct session_struct *session);

__access_no_size(read_only, 1)
void sig_orphan_pgrp(const struct proc_orphan_event *event);

__access_no_size(read_only, 1)
void sig_notify_parent(const struct proc_parent_event *event);

int sig_send_self(int sig);

__must_check
__access_no_size(read_only, 2) __access_no_size(read_write, 3)
int sig_force_info(int sig, const siginfo_t *info, struct task_struct *task);

__must_check __access_no_size(read_only, 1)
bool sig_fatal_pending(struct task_struct *task);

__must_check __access_no_size(read_only, 1)
bool sig_wait_ready(const struct task_struct *task, const struct task_wait *wait);

__must_check __access_no_size(read_write, 1)
int sig_task_init(struct task_struct *task);

__must_check __access_no_size(read_write, 1)
int sig_proc_init(struct proc_struct *proc);

__must_check __access_no_size(read_only, 1)
struct sigchld_exit_policy sigchld_exit_policy(const struct proc_struct *proc);

__must_check __access_no_size(read_write, 1)
int sig_task_clone(struct task_struct *child, bool share_sighand, bool disable_altstack);

/**
 * @brief Prepare the private signal-handler table required by exec.
 * @param task Task replacing its image.
 * @param prepared Receives the uncommitted handler table.
 * @return 0, or a negative errno.
 *
 * The current proc handler table is not modified.  The caller must either
 * commit or abort the returned table.
 */
__must_check
__access_no_size(read_only, 1) __access_no_size( write_only, 2)
int sig_exec_prepare(const struct task_struct *task, struct sighand_struct **prepared);

__nonnull(1, 2) __access_no_size(read_write, 1)
void sig_exec_commit(struct task_struct *task, struct sighand_struct *prepared);

__access_no_size(read_write, 1)
void sig_exec_abort(struct sighand_struct *prepared);

__access_no_size(read_write, 1)
void sig_task_release(struct task_struct *task);

__access_no_size(read_write, 1)
void sig_proc_release(struct proc_struct *proc);

/**
 * @brief Deliver one pending signal before returning to userspace.
 * @param tf User trap frame to rewrite for handler entry.
 */
__hot __nonnull(1) __access_no_size(read_write, 1)
void sig_deliver(struct trap_frame *tf);

/**
 * @brief Register the fixed signal trampoline user mapping.
 */
__cold
void sig_init(void);

/**
 * @brief Implement Linux kill() process and process-group signal semantics.
 * @param pid Positive process ID, zero for the caller's process group, -1 for
 *        the supported broadcast policy, or a negative process-group ID.
 * @param sig Signal number, or 0 for permission/existence probe.
 * @return 0 on success, or a negative errno.
 */
__must_check
int sig_kill(pid_t pid, int sig);

/**
 * @brief Implement tkill() thread-directed signal semantics.
 * @param tid Target thread id.
 * @param sig Signal number.
 * @return 0 on success, or a negative errno.
 */
__must_check
int sig_tkill(pid_t tid, int sig);

/**
 * @brief Implement tgkill() thread-group-qualified signal semantics.
 * @param tgid Target thread-group id.
 * @param tid Target thread id.
 * @param sig Signal number.
 * @return 0 on success, or a negative errno.
 */
__must_check
int sig_tgkill(pid_t tgid, pid_t tid, int sig);

/**
 * @brief Register or query the current task alternate signal stack.
 * @param ss Optional new userspace stack_t.
 * @param old_ss Optional output for previous stack_t.
 * @return 0 on success, or a negative errno.
 */
__must_check
__access_no_size(read_only, 1) __access_no_size(write_only, 2)
int sig_altstack(const struct stack_t *ss, struct stack_t *old_ss);

/**
 * @brief Install or query one signal action.
 * @param sig Signal number.
 * @param act Optional new action.
 * @param oldact Optional output for the action atomically replaced by @p act.
 * @return 0 on success, or a negative errno.
 */
__must_check
__access_no_size(read_only, 2) __access_no_size(write_only, 3)
int sig_action(int sig, const struct sigaction *act, struct sigaction *oldact);

/**
 * @brief Apply Linux rt_sigprocmask operation to the current task.
 * @param how SIG_BLOCK, SIG_UNBLOCK, or SIG_SETMASK.
 * @param set Optional new mask.
 * @param oldset Optional output for previous mask.
 * @return 0 on success, or a negative errno.
 */
__must_check
__access_no_size(read_only, 2) __access_no_size(write_only, 3)
int sig_procmask(int how, const uint64_t *set, uint64_t *oldset);

/**
 * @brief Temporarily replace the signal mask and wait for interruption.
 * @param mask Replacement blocked signal mask.
 * @return Always -EINTR after an unblocked signal is observed, or a negative
 *         errno from the wait subsystem.
 *
 * The previous mask is restored through the signal-frame path after a caught
 * signal handler returns to userspace.
 */
__must_check
int sig_suspend(uint64_t mask);

/**
 * @brief Synchronously consume or wait for a pending signal in a set.
 * @param set Signals accepted by the wait.
 * @param timeout Optional relative timeout; NULL waits indefinitely.
 * @param info Consumed signal information.
 * @return Signal number on success, or a negative errno.
 */
__must_check __nonnull(3)
__access_no_size(read_only, 2) __access_no_size(write_only, 3)
int sig_wait(uint64_t set, const struct timespec *timeout, siginfo_t *info);

/**
 * @brief Restore a userspace context from a signal frame.
 * @param tf Current syscall trap frame.
 * @param sp User stack pointer pointing at the current frame-stack top.
 * @return The restored a0 value for syscall dispatch to write back.
 */
__must_check __nonnull(1) __access_no_size(read_write, 1)
ssize_t sig_return(struct trap_frame *tf, uintptr_t sp);

/* Syscall-restart context: task-owned signal state. The dispatcher saves
 * through restart_save(); the signal module owns the restartable policy and
 * the signal-delivery decision. */
__nonnull(1, 2)
__access_no_size(read_write, 1) __access_no_size(read_only, 2)
void restart_save(struct task_struct *task, const struct trap_frame *tf, size_t nr);

__nonnull(1) __access_no_size(read_write, 1)
void restart_finish(struct task_struct *task, ssize_t ret);

__access_no_size(read_write, 1)
void restart_clear(struct task_struct *task);

__nonnull(2)
__access_no_size(read_write, 1) __access_no_size(read_write, 2)
bool restart_for_signal(struct task_struct *task, struct trap_frame *tf, bool sa_restart);

#endif
