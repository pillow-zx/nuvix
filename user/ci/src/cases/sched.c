/*
 * Scheduler seam regression: affinity placement, blocked-task migration,
 * clone-mask inheritance, and the temporary CPU-0-only membarrier seam.
 *
 * musl has no sched_setaffinity/sched_getaffinity wrappers: raw syscalls.
 */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/membarrier.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <utest.h>

#define SCHED_POLL_BUDGET_MS 2000

static void sched_sleep_ms(long milliseconds)
{
	struct timespec delay = {
		.tv_sec = milliseconds / 1000,
		.tv_nsec = (milliseconds % 1000) * 1000000L,
	};

	while (nanosleep(&delay, &delay) < 0)
		;
}

UT_CASE(sched_affinity_get_set_self_errors, 5000)
{
	unsigned long mask = 0;
	unsigned long current_mask;
	unsigned long other_mask;

	UT_ASSERT_EQ(syscall(SYS_sched_getaffinity, 0, sizeof(mask), &mask),
		     (long)sizeof(mask));
	UT_ASSERT(mask == 1UL || mask == 2UL);
	current_mask = mask;

	UT_ASSERT_ERRNO(syscall(SYS_sched_getaffinity, 0, 4, &mask), EINVAL);
	UT_ASSERT_ERRNO(syscall(SYS_sched_getaffinity, 0, sizeof(mask),
				(void *)1), EFAULT);

	/* Self is running (on_cpu): affinity change is rejected. */
	UT_ASSERT_ERRNO(syscall(SYS_sched_setaffinity, 0, sizeof(mask), &mask),
			EBUSY);
	/* Empty masks stay invalid. Any valid mask is busy for the running self. */
	mask = 0;
	UT_ASSERT_ERRNO(syscall(SYS_sched_setaffinity, 0, sizeof(mask), &mask),
			EINVAL);
	other_mask = current_mask == 1UL ? 2UL : 1UL;
	UT_ASSERT_ERRNO(syscall(SYS_sched_setaffinity, 0, sizeof(other_mask),
				&other_mask), EBUSY);
	mask = 3;
	UT_ASSERT_ERRNO(syscall(SYS_sched_setaffinity, 0, sizeof(mask), &mask),
			EBUSY);

	/* Unknown targets. */
	UT_ASSERT_ERRNO(syscall(SYS_sched_getaffinity, 999, sizeof(mask),
				&mask), ESRCH);
	UT_ASSERT_ERRNO(syscall(SYS_sched_setaffinity, -1, sizeof(mask), &mask),
			ESRCH);
}

UT_CASE(sched_affinity_blocked_migrate, 5000)
{
	int ready_pipe[2];
	int release_pipe[2];
	int report_pipe[2];
	pid_t child;
	unsigned long mask = 0;
	unsigned long target_mask;
	char byte;

	/* The child announces readiness on ready_pipe[1], then blocks reading
	 * release_pipe[0] until the parent releases it after changing affinity. */
	UT_ASSERT_EQ(pipe(ready_pipe), 0);
	UT_ASSERT_EQ(pipe(release_pipe), 0);
	UT_ASSERT_EQ(pipe(report_pipe), 0);
	UT_ASSERT_EQ(syscall(SYS_sched_getaffinity, 0, sizeof(mask), &mask),
		     (long)sizeof(mask));
	UT_ASSERT(mask == 1UL || mask == 2UL);
	target_mask = mask == 1UL ? 2UL : 1UL;
	child = UT_FORK();
	if (child == 0) {
		unsigned long child_mask = 0;

		(void)close(ready_pipe[0]);
		(void)close(release_pipe[1]);
		(void)close(report_pipe[0]);
		(void)write(ready_pipe[1], "b", 1);
		if (read(release_pipe[0], &byte, 1) != 1)
			_exit(127);
		if (syscall(SYS_sched_getaffinity, 0, sizeof(child_mask),
			    &child_mask) != (long)sizeof(child_mask))
			_exit(126);
		if (write(report_pipe[1], &child_mask, sizeof(child_mask)) !=
		    (ssize_t)sizeof(child_mask))
			_exit(125);
		_exit(0);
	}
	(void)close(ready_pipe[1]);
	(void)close(release_pipe[0]);
	(void)close(report_pipe[1]);
	UT_ASSERT_EQ(read(ready_pipe[0], &byte, 1), 1);
	(void)close(ready_pipe[0]);

	/* The child transitions to TASK_BLOCKED asynchronously; poll the
	 * setaffinity call until it succeeds (a running child gets EBUSY). */
	{
		int tries;

		for (tries = 0; tries < SCHED_POLL_BUDGET_MS / 5; tries++) {
			errno = 0;
			if (syscall(SYS_sched_setaffinity, child,
					    sizeof(target_mask), &target_mask) == 0)
				break;
			UT_ASSERT_EQ(errno, EBUSY);
			sched_sleep_ms(5);
		}
		UT_ASSERT(tries < SCHED_POLL_BUDGET_MS / 5);
	}

	mask = 0;
	UT_ASSERT_EQ(syscall(SYS_sched_getaffinity, child, sizeof(mask),
			     &mask), (long)sizeof(mask));
	UT_ASSERT_EQ(mask, target_mask);

	UT_ASSERT_EQ(write(release_pipe[1], "r", 1), 1);
	UT_ASSERT_EQ(close(release_pipe[1]), 0);
	UT_ASSERT_EQ(read(report_pipe[0], &mask, sizeof(mask)),
		     (ssize_t)sizeof(mask));
	UT_ASSERT_EQ(mask, target_mask);
	UT_ASSERT_EQ(close(report_pipe[0]), 0);
	UT_EXPECT_EXIT(child, 0);
}

UT_CASE(sched_affinity_ebusy_while_running, 5000)
{
	unsigned long mask = 0;

	UT_ASSERT_EQ(syscall(SYS_sched_getaffinity, 0, sizeof(mask), &mask),
		     (long)sizeof(mask));
	UT_ASSERT_ERRNO(syscall(SYS_sched_setaffinity, 0, sizeof(mask), &mask),
			EBUSY);
}

UT_CASE(sched_affinity_mask_inherit, 5000)
{
	unsigned long mask = 0;
	pid_t child;

	UT_ASSERT_EQ(syscall(SYS_sched_getaffinity, 0, sizeof(mask), &mask),
		     (long)sizeof(mask));
	child = UT_FORK();
	if (child == 0) {
		pid_t grandchild = fork();
		unsigned long grandchild_mask = 0;
		int status;

		if (grandchild < 0)
			_exit(127);
		if (grandchild == 0) {
			if (syscall(SYS_sched_getaffinity, 0,
				    sizeof(grandchild_mask), &grandchild_mask) !=
			    (long)sizeof(grandchild_mask) || grandchild_mask != mask)
				_exit(126);
			_exit(0);
		}
		status = UT_WAIT_CHILD(grandchild);
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
			_exit(125);
		_exit(0);
	}
	UT_EXPECT_EXIT(child, 0);
}

UT_CASE(sched_affinity_cross_user_eperm, 5000)
{
	unsigned long mask = 1;
	pid_t child;

	child = UT_FORK();
	if (child == 0) {
		if (setuid(1000) != 0)
			_exit(127);
		errno = 0;
		if (syscall(SYS_sched_setaffinity, getppid(), sizeof(mask),
			    &mask) != -1 || errno != EPERM)
			_exit(126);
		/* nuvix divergence: getaffinity has no uid gate (Linux
		 * returns EPERM for a cross-user read). */
		if (syscall(SYS_sched_getaffinity, getppid(), sizeof(mask),
			    &mask) != (long)sizeof(mask))
			_exit(125);
		_exit(0);
	}
	UT_EXPECT_EXIT(child, 0);
}

UT_CASE(sched_membarrier_cpu_flag_single_core, 5000)
{
	UT_ASSERT(syscall(SYS_membarrier, MEMBARRIER_CMD_QUERY, 0, 0) > 0);
	UT_ASSERT_EQ(syscall(SYS_membarrier,
			     MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ,
			     0, 0), 0);
	UT_ASSERT_EQ(syscall(SYS_membarrier,
			     MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ,
			     MEMBARRIER_CMD_FLAG_CPU, 0), 0);
	/* CPU 0 remains the only accepted target until remote barriers exist. */
	UT_ASSERT_ERRNO(syscall(SYS_membarrier,
				MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ,
				MEMBARRIER_CMD_FLAG_CPU, 1), EINVAL);
	UT_ASSERT_ERRNO(syscall(SYS_membarrier,
				MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ,
				MEMBARRIER_CMD_FLAG_CPU, -1), EINVAL);
	/* Unregistered private expedited command. */
	UT_ASSERT_ERRNO(syscall(SYS_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED,
				0, 0), EPERM);
}
