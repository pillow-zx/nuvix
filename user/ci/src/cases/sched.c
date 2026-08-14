/*
 * Scheduler seam regression: affinity storage/roundtrip,
 * -EBUSY/-EINVAL/-EPERM/-ESRCH paths, and the single-core membarrier
 * FLAG_CPU seam (online-CPU check, no hardcoded CPU 0).
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

	UT_ASSERT_EQ(syscall(SYS_sched_getaffinity, 0, sizeof(mask), &mask),
		     (long)sizeof(mask));
	UT_ASSERT_EQ(mask, 1UL);

	UT_ASSERT_ERRNO(syscall(SYS_sched_getaffinity, 0, 4, &mask), EINVAL);
	UT_ASSERT_ERRNO(syscall(SYS_sched_getaffinity, 0, sizeof(mask),
				(void *)1), EFAULT);

	/* Self is running (on_cpu): affinity change is rejected. */
	mask = 1;
	UT_ASSERT_ERRNO(syscall(SYS_sched_setaffinity, 0, sizeof(mask), &mask),
			EBUSY);
	/* Empty mask / offline-only mask. */
	mask = 0;
	UT_ASSERT_ERRNO(syscall(SYS_sched_setaffinity, 0, sizeof(mask), &mask),
			EINVAL);
	mask = 2;
	UT_ASSERT_ERRNO(syscall(SYS_sched_setaffinity, 0, sizeof(mask), &mask),
			EINVAL);
	/* Bit 0 survives the online intersection, then EBUSY. */
	mask = 3;
	UT_ASSERT_ERRNO(syscall(SYS_sched_setaffinity, 0, sizeof(mask), &mask),
			EBUSY);

	/* Unknown targets. */
	UT_ASSERT_ERRNO(syscall(SYS_sched_getaffinity, 999, sizeof(mask),
				&mask), ESRCH);
	UT_ASSERT_ERRNO(syscall(SYS_sched_setaffinity, -1, sizeof(mask), &mask),
			ESRCH);
}

UT_CASE(sched_affinity_set_blocked_child, 5000)
{
	int ready_pipe[2];
	int release_pipe[2];
	pid_t child;
	unsigned long mask = 1;
	char byte;

	/* Two pipes: the child announces readiness on ready_pipe[1] and
	 * blocks reading release_pipe[0] until the parent closes its write
	 * end. A single pipe would let the child consume its own byte. */
	UT_ASSERT_EQ(pipe(ready_pipe), 0);
	UT_ASSERT_EQ(pipe(release_pipe), 0);
	child = UT_FORK();
	if (child == 0) {
		char c = 0;

		(void)close(ready_pipe[0]);
		(void)close(release_pipe[1]);
		(void)write(ready_pipe[1], "b", 1);
		if (read(release_pipe[0], &c, 1) != 0)
			_exit(127);
		_exit(0);
	}
	(void)close(ready_pipe[1]);
	(void)close(release_pipe[0]);
	UT_ASSERT_EQ(read(ready_pipe[0], &byte, 1), 1);
	(void)close(ready_pipe[0]);

	/* The child transitions to TASK_BLOCKED asynchronously; poll the
	 * setaffinity call until it succeeds (a running child gets EBUSY). */
	{
		int tries;

		for (tries = 0; tries < SCHED_POLL_BUDGET_MS / 5; tries++) {
			errno = 0;
			if (syscall(SYS_sched_setaffinity, child,
				    sizeof(mask), &mask) == 0)
				break;
			sched_sleep_ms(5);
		}
		UT_ASSERT(tries < SCHED_POLL_BUDGET_MS / 5);
	}

	mask = 2;
	UT_ASSERT_ERRNO(syscall(SYS_sched_setaffinity, child, sizeof(mask),
				&mask), EINVAL);
	mask = 3;
	UT_ASSERT_EQ(syscall(SYS_sched_setaffinity, child, sizeof(mask),
			     &mask), 0);
	mask = 0;
	UT_ASSERT_EQ(syscall(SYS_sched_getaffinity, child, sizeof(mask),
			     &mask), (long)sizeof(mask));
	UT_ASSERT_EQ(mask, 1UL);

	UT_ASSERT_EQ(close(release_pipe[1]), 0);
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
	/* Offline CPU and negative CPU id are rejected (no hardcoded 0). */
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
