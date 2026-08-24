#ifndef _NUVIX_EXIT_H
#define _NUVIX_EXIT_H

/**
 * @file exit.h
 * @brief 进程退出与 wait4 回收接口。
 */

#include <nuvix/types.h>
#include <nuvix/task.h>
#include <nuvix/proc.h>

/**
 * @brief Terminate the current task with a normal userspace exit code.
 * @param code Userspace exit code; only its low 8 bits are wait-visible.
 */
__noreturn
void do_exit(int code);

/**
 * @brief Terminate every task in the current thread group with one exit code.
 * @param code Userspace exit code; only its low 8 bits are wait-visible.
 */
__noreturn
void do_exit_group(int code);

/**
 * @brief Terminate the current task because a signal used its default action.
 * @param sig Linux signal number exposed through WTERMSIG.
 */
__noreturn
void do_exit_signal(int sig);

/** Start the non-exiting kernel reaper task. */
int task_reaper_start(void);

/**
 * @struct wait4_result
 * @brief Deferred wait4 result whose task release is finished by caller.
 *
 * @par Fields
 * - @c claim: Task-owned child event held until userspace copies succeed.
 * - @c cputime: Total child CPU-time snapshot returned through rusage.
 * - @c pid: Wait result pid.
 * - @c status: Linux wait status.
 */
struct wait4_result {
	struct proc_wait_claim claim;
	struct task_cputime cputime;
	pid_t pid;
	int status;
};

/**
 * @brief Release a zombie task after it is no longer waitable.
 * @param task Zombie task to release.
 */
void release_task(struct task_struct *task);

/**
 * @brief Wait for a child process according to Linux wait4 pid/options.
 * @param pid pid selector accepted by the current implementation.
 * @param options Linux wait options.
 * @param result Output result; finalized by kernel_wait4_finish().
 * @return 0 when @p result was filled (including a WNOHANG empty result), or
 * a negative errno.
 */
int kernel_wait4(pid_t pid, int options, struct wait4_result *result);

/**
 * @brief Finish one wait4 result transaction.
 * @param result Result previously filled by kernel_wait4().
 *
 * This is terminal: it commits when possible, releases caller-owned orphan
 * storage on either commit result, and clears @p result. Call at most once.
 */
void kernel_wait4_finish(struct wait4_result *result);

/**
 * @brief Abort one wait4 result transaction after a userspace-copy failure.
 * @param result Result previously filled by kernel_wait4().
 *
 * This is terminal: it aborts a live claim when present, releases orphan
 * storage even for an empty result, and clears @p result. Call at most once.
 */
void kernel_wait4_abort(struct wait4_result *result);

#endif
