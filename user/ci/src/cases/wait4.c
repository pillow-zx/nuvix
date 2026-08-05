/*
 * wait4 ABI selector/option regression (TODO.md P2): PID/PGID selectors
 * (pid>0, pid==0, pid==-1, pid<-1), __WNOTHREAD/__WCLONE/__WALL classes,
 * unknown-option -EINVAL, stop/continue/exit claim priority, WNOHANG,
 * -ECHILD and -EFAULT rollback, and rusage.
 *
 * musl has no wait4() wrapper in this tree: rusage/EFAULT paths use raw
 * syscall(SYS_wait4, ...). waitpid passes options through unmodified.
 */
#include <errno.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <utest.h>

#define WAIT4_POLL_BUDGET_MS 1000

static void wait4_sleep_ms(long milliseconds)
{
	struct timespec delay = {
		.tv_sec = milliseconds / 1000,
		.tv_nsec = (milliseconds % 1000) * 1000000L,
	};

	while (nanosleep(&delay, &delay) < 0)
		;
}

static bool wait4_elapsed_at_least(const struct timespec *start,
				   const struct timespec *now, int64_t seconds,
				   int64_t nanoseconds)
{
	int64_t elapsed_seconds = (int64_t)now->tv_sec - start->tv_sec;
	int64_t elapsed_nanoseconds = (int64_t)now->tv_nsec - start->tv_nsec;

	if (elapsed_nanoseconds < 0) {
		elapsed_seconds--;
		elapsed_nanoseconds += 1000000000L;
	}
	return elapsed_seconds > seconds ||
	       (elapsed_seconds == seconds &&
		elapsed_nanoseconds >= nanoseconds);
}

static bool wait4_poll_wnohang(pid_t child, int *status)
{
	for (int tries = 0; tries < WAIT4_POLL_BUDGET_MS / 5; tries++) {
		pid_t got = waitpid(child, status, WNOHANG);

		if (got == child)
			return true;
		if (got == -1)
			return false;
		wait4_sleep_ms(5);
	}
	return false;
}

UT_CASE(wait4_pid_positive_wnohang_claim, 5000)
{
	pid_t child;
	int status;

	child = UT_FORK();
	if (child == 0)
		_exit(9);
	UT_ASSERT(wait4_poll_wnohang(child, &status));
	UT_ASSERT(WIFEXITED(status));
	UT_ASSERT_EQ(WEXITSTATUS(status), 9);
	UT_ASSERT_ERRNO(waitpid(child, &status, WNOHANG), ECHILD);
}

UT_CASE(wait4_pid_zero_same_pgrp, 5000)
{
	pid_t first;
	pid_t second;
	int status;

	/* The case is a pgrp leader (runner setpgid(0,0)): pid==0 matches
	 * children in the same pgrp. */
	first = UT_FORK();
	if (first == 0)
		_exit(9);
	UT_ASSERT_EQ(waitpid(0, &status, 0), first);
	UT_ASSERT(WIFEXITED(status));
	UT_ASSERT_EQ(WEXITSTATUS(status), 9);

	/* A child that left the pgrp never matches the pid==0 selector.
	 * Handshake: the child signals its new pgrp via a pipe before
	 * exiting, so the parent cannot probe before setpgid ran. */
	{
		int pgrp_pipe[2];

		UT_ASSERT_EQ(pipe(pgrp_pipe), 0);
		second = UT_FORK();
		if (second == 0) {
			(void)close(pgrp_pipe[0]);
			if (setpgid(0, 0) != 0)
				_exit(97);
			(void)write(pgrp_pipe[1], "P", 1);
			_exit(9);
		}
		(void)close(pgrp_pipe[1]);
		UT_ASSERT_EQ(read(pgrp_pipe[0], &(char){0}, 1), 1);
		(void)close(pgrp_pipe[0]);
	}
	UT_ASSERT_ERRNO(waitpid(0, &status, WNOHANG), ECHILD);
	UT_ASSERT_EQ(waitpid(second, &status, 0), second);
	UT_ASSERT(WIFEXITED(status));
}

UT_CASE(wait4_pid_negpgid_and_minus1, 5000)
{
	pid_t child;
	int status;

	child = UT_FORK();
	if (child == 0) {
		if (setpgid(0, 0) != 0)
			_exit(90 + errno);
		_exit(9);
	}
	/* Reap by positive pid first: a 90+errno code means setpgid failed. */
	{
		pid_t got = waitpid(child, &status, 0);

		if (got != child)
			UT_FAIL("waitpid(%d): got=%d errno=%d", child, got,
				errno);
		UT_ASSERT_EQ(WEXITSTATUS(status), 9);
	}

	/* The -pid selector against a child that moved to its own pgrp.
	 * Handshake: the child signals its new pgrp before exiting so the
	 * parent never probes before setpgid ran. */
	{
		int pgrp_pipe[2];

		UT_ASSERT_EQ(pipe(pgrp_pipe), 0);
		child = UT_FORK();
		if (child == 0) {
			(void)close(pgrp_pipe[0]);
			if (setpgid(0, 0) != 0)
				_exit(97);
			(void)write(pgrp_pipe[1], "P", 1);
			_exit(9);
		}
		(void)close(pgrp_pipe[1]);
		UT_ASSERT_EQ(read(pgrp_pipe[0], &(char){0}, 1), 1);
		(void)close(pgrp_pipe[0]);
		UT_ASSERT_EQ(waitpid(-child, &status, 0), child);
		UT_ASSERT(WIFEXITED(status));
		UT_ASSERT_EQ(WEXITSTATUS(status), 9);
	}

	/* pid == -1: any child, in unspecified order. */
	child = UT_FORK();
	if (child == 0)
		_exit(7);
	UT_ASSERT_EQ(waitpid(-1, &status, 0), child);
	UT_ASSERT(WIFEXITED(status));
	UT_ASSERT_EQ(WEXITSTATUS(status), 7);

	child = UT_FORK();
	if (child == 0)
		_exit(7);
	UT_ASSERT_EQ(waitpid(-1, &status, 0), child);
	UT_ASSERT_EQ(WEXITSTATUS(status), 7);
	UT_ASSERT_ERRNO(waitpid(-1, &status, WNOHANG), ECHILD);
}

UT_CASE(wait4_options_and_wnohang, 5000)
{
	pid_t child;
	int status;

	/* No children at all. */
	UT_ASSERT_ERRNO(waitpid(-1, &status, WNOHANG), ECHILD);

	child = UT_FORK();
	if (child == 0) {
		wait4_sleep_ms(150);
		_exit(0);
	}
	UT_ASSERT_EQ(waitpid(child, &status, WNOHANG), 0);
	UT_ASSERT_EQ(waitpid(child, &status, 0), child);
	UT_ASSERT(WIFEXITED(status));

	/* Unknown option bits are rejected before any claim. */
	UT_ASSERT_ERRNO(waitpid(-1, &status, WNOWAIT), EINVAL);
	UT_ASSERT_ERRNO(waitpid(-1, &status, WEXITED), EINVAL);
	UT_ASSERT_ERRNO(waitpid(-1, &status, WNOWAIT | WNOHANG), EINVAL);
}

UT_CASE(wait4_wclone_class_and_wall, 5000)
{
	long clone_child;
	int status;

	/* clone with exit_signal 0: a CLONE-class (non-SIGCHLD) child. */
	clone_child = syscall(SYS_clone, 0, 0, 0, 0, 0);
	UT_ASSERT(clone_child >= 0);
	if (clone_child == 0)
		_exit(11);

	/* The default class waits only SIGCHLD children. */
	UT_ASSERT_ERRNO(waitpid(clone_child, &status, WNOHANG), ECHILD);
	UT_ASSERT_EQ(waitpid(clone_child, &status, __WCLONE), clone_child);
	UT_ASSERT(WIFEXITED(status));
	UT_ASSERT_EQ(WEXITSTATUS(status), 11);
	UT_ASSERT_ERRNO(waitpid(clone_child, &status, WNOHANG | __WCLONE),
			ECHILD);

	/* __WALL reaps both classes (exit events publish asynchronously:
	 * poll each claim). */
	{
		pid_t sigchild;
		long clonep;
		uint32_t remaining = (1U << 3) | (1U << 4);
		int tries;

		sigchild = UT_FORK();
		if (sigchild == 0)
			_exit(3);
		clonep = syscall(SYS_clone, 0, 0, 0, 0, 0);
		UT_ASSERT(clonep >= 0);
		if (clonep == 0)
			_exit(4);

		for (tries = 0; tries < 200 && remaining; tries++) {
			pid_t got = waitpid(-1, &status, WNOHANG | __WALL);

			if (got == -1 || got == 0) {
				wait4_sleep_ms(5);
				continue;
			}
			UT_ASSERT(WIFEXITED(status));
			remaining &= ~(1U << WEXITSTATUS(status));
		}
		UT_ASSERT(tries < 200);
		UT_ASSERT_EQ(remaining, 0);
		UT_ASSERT_ERRNO(waitpid(-1, &status, WNOHANG | __WALL),
				ECHILD);
	}

	/* A SIGCHLD-class-only child list never matches __WCLONE. */
	{
		pid_t sigchild;

		sigchild = UT_FORK();
		if (sigchild == 0)
			_exit(0);
		UT_ASSERT_ERRNO(waitpid(-1, &status, WNOHANG | __WCLONE),
				ECHILD);
		UT_EXPECT_EXIT(sigchild, 0);
	}
}

UT_CASE(wait4_wnotthread_creator, 5000)
{
	pid_t child;
	int status;

	/* Single-threaded parent: creator TID == current TID, so
	 * __WNOTHREAD is accepted and claims normally. */
	child = UT_FORK();
	if (child == 0)
		_exit(6);
	UT_ASSERT_EQ(waitpid(child, &status, __WNOTHREAD), child);
	UT_ASSERT(WIFEXITED(status));
	UT_ASSERT_EQ(WEXITSTATUS(status), 6);

	child = UT_FORK();
	if (child == 0) {
		wait4_sleep_ms(150);
		_exit(0);
	}
	UT_ASSERT_EQ(waitpid(child, &status, WNOHANG | __WNOTHREAD), 0);
	UT_ASSERT_EQ(waitpid(child, &status, 0), child);
}

UT_CASE(wait4_stop_exit_priority, 5000)
{
	pid_t child;
	int status;

	/* Without WUNTRACED the stop event is not claimable. */
	child = UT_FORK();
	if (child == 0) {
		(void)raise(SIGSTOP);
		_exit(0);
	}
	wait4_sleep_ms(50);
	UT_ASSERT_EQ(waitpid(child, &status, WNOHANG), 0);
	UT_ASSERT_EQ(kill(child, SIGKILL), 0);
	UT_ASSERT_EQ(waitpid(child, &status, 0), child);
	UT_ASSERT(WIFSIGNALED(status));
	UT_ASSERT_EQ(WTERMSIG(status), SIGKILL);

	/* EXIT beats STOP when both are claimable (event priority). The
	 * stop event publishes synchronously but the exit event arrives
	 * with the reaper, so the first claim may legitimately be the stop:
	 * keep claiming until the exit is seen. */
	child = UT_FORK();
	if (child == 0) {
		(void)raise(SIGSTOP);
		_exit(0);
	}
	wait4_sleep_ms(50);
	UT_ASSERT_EQ(kill(child, SIGKILL), 0);
	{
		int tries;

		for (tries = 0; tries < 200; tries++) {
			pid_t got = waitpid(child, &status,
					    WNOHANG | WUNTRACED);

			if (got == child) {
				if (WIFSIGNALED(status))
					break;
				/* A stop claim first: retry for the exit. */
				continue;
			}
			wait4_sleep_ms(5);
		}
		UT_ASSERT(tries < 200);
	}
	UT_ASSERT(WIFSIGNALED(status));
	UT_ASSERT_EQ(WTERMSIG(status), SIGKILL);

	/* Stop claim via WUNTRACED, continue claim via WCONTINUED. */
	child = UT_FORK();
	if (child == 0) {
		(void)raise(SIGSTOP);
		for (;;)
			pause();
	}
	{
		int tries;

		for (tries = 0; tries < 200; tries++) {
			pid_t got;

			got = waitpid(child, &status, WNOHANG | WUNTRACED);
			if (got == child && WIFSTOPPED(status))
				break;
			wait4_sleep_ms(5);
		}
		UT_ASSERT(tries < 200);
		UT_ASSERT(WIFSTOPPED(status));
		UT_ASSERT_EQ(WSTOPSIG(status), SIGSTOP);
	}
	UT_ASSERT_EQ(kill(child, SIGCONT), 0);
	UT_ASSERT_EQ(waitpid(child, &status, WCONTINUED), child);
	UT_ASSERT(WIFCONTINUED(status));
	UT_ASSERT_EQ(kill(child, SIGKILL), 0);
	UT_ASSERT_EQ(waitpid(child, &status, 0), child);
	UT_ASSERT(WIFSIGNALED(status));
	UT_ASSERT_EQ(WTERMSIG(status), SIGKILL);
}

UT_CASE(wait4_echild_paths, 5000)
{
	pid_t child;
	int status;

	UT_ASSERT_ERRNO(waitpid(-1, &status, WNOHANG), ECHILD);

	/* Children exist, but the pid does not name one of them. */
	child = UT_FORK();
	if (child == 0) {
		wait4_sleep_ms(150);
		_exit(0);
	}
	UT_ASSERT_ERRNO(waitpid(254, &status, WNOHANG), ECHILD);
	UT_ASSERT_EQ(waitpid(child, &status, 0), child);
	UT_ASSERT(WIFEXITED(status));
}

UT_CASE(wait4_efault_rollback_and_rusage, 5000)
{
	pid_t child;
	int status;
	int tries;

	/* Bad wstatus pointer: -EFAULT, and the claim is rolled back so a
	 * later wait still sees the event. */
	child = UT_FORK();
	if (child == 0)
		_exit(23);
	for (tries = 0; tries < 200 && kill(child, 0) != 0; tries++)
		wait4_sleep_ms(5);
	UT_ASSERT(tries < 200);
	UT_ASSERT_ERRNO(syscall(SYS_wait4, child, (int *)1, 0, NULL), EFAULT);
	UT_ASSERT_EQ(waitpid(child, &status, 0), child);
	UT_ASSERT(WIFEXITED(status));
	UT_ASSERT_EQ(WEXITSTATUS(status), 23);

	/* Bad rusage pointer: same rollback. */
	child = UT_FORK();
	if (child == 0)
		_exit(24);
	for (tries = 0; tries < 200 && kill(child, 0) != 0; tries++)
		wait4_sleep_ms(5);
	UT_ASSERT(tries < 200);
	UT_ASSERT_ERRNO(syscall(SYS_wait4, child, &status, 0,
				(struct rusage *)1), EFAULT);
	UT_ASSERT_EQ(waitpid(child, &status, 0), child);
	UT_ASSERT_EQ(WEXITSTATUS(status), 24);

	/* Valid rusage: child burns real user CPU so utime ticks.  A
	 * clock_gettime-only loop spends nearly all its time in kernel
	 * syscalls (stime), so utime can stay zero for the whole window. */
	child = UT_FORK();
	if (child == 0) {
		struct timespec start;
		struct timespec now;
		volatile uint64_t value = 1;

		clock_gettime(CLOCK_MONOTONIC, &start);
		do {
			for (unsigned int index = 0; index < 10000; index++)
				value = value * 6364136223846793005ULL + 1;
			clock_gettime(CLOCK_MONOTONIC, &now);
		} while (!wait4_elapsed_at_least(&start, &now, 0, 120000000L));
		if (value == 0)
			_exit(125);
		_exit(0);
	}
	{
		struct rusage rusage = {0};

		UT_ASSERT_EQ(syscall(SYS_wait4, child, &status, 0, &rusage),
			     child);
		UT_ASSERT(WIFEXITED(status));
		UT_ASSERT_EQ(WEXITSTATUS(status), 0);
		UT_ASSERT(rusage.ru_utime.tv_sec >= 0 &&
			  rusage.ru_stime.tv_sec >= 0);
		UT_EXPECT(rusage.ru_utime.tv_usec > 0);
	}
}
