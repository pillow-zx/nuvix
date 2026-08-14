/*
 * Reaper/TID lifecycle regression: zombie visibility, TID/PID
 * release after claim, SIG_IGN / SA_NOCLDWAIT auto-reap with immediate PID
 * reuse, fork/exit storm reaping, and PID role retention until claim.
 *
 * The kernel publishes exit events asynchronously (the reaper thread runs
 * release_task), so every "ready" check polls with a bounded budget instead
 * of sleeping-and-assuming. All cases are single-threaded.
 */
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <utest.h>

#define REAPER_POLL_BUDGET_MS 1000

static void reaper_sleep_ms(long milliseconds)
{
	struct timespec delay = {
		.tv_sec = milliseconds / 1000,
		.tv_nsec = (milliseconds % 1000) * 1000000L,
	};

	while (nanosleep(&delay, &delay) < 0)
		;
}

static bool reaper_poll_until(bool (*predicate)(void *data), void *data)
{
	for (int tries = 0; tries < REAPER_POLL_BUDGET_MS / 5; tries++) {
		if (predicate(data))
			return true;
		reaper_sleep_ms(5);
	}
	return false;
}

static bool reaper_zombie_visible(void *data)
{
	pid_t pid = (pid_t)(uintptr_t)data;

	return kill(pid, 0) == 0;
}

static bool reaper_echild(void *data)
{
	int status;

	(void)data;
	errno = 0;
	return waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD;
}

struct reaper_claim {
	pid_t pid;
	int *status;
};

/* Exit events publish asynchronously (reaper thread): poll the actual
 * claim, not the proc's visibility. */
static bool reaper_claimable(void *data)
{
	struct reaper_claim *claim = data;

	return waitpid(claim->pid, claim->status, WNOHANG) == claim->pid;
}

UT_CASE(reaper_zombie_visibility_and_tid_release, 5000)
{
	pid_t child;
	int status;

	child = UT_FORK();
	if (child == 0)
		_exit(7);
	{
		struct reaper_claim claim = {.pid = child, .status = &status};

		UT_ASSERT(reaper_poll_until(reaper_claimable, &claim));
	}

	/* Zombie is claimable; commit releases the identity. */
	UT_ASSERT(WIFEXITED(status));
	UT_ASSERT_EQ(WEXITSTATUS(status), 7);

	/* After the claim the TID/proc role is gone. */
	errno = 0;
	UT_ASSERT(kill(child, 0) == -1 && errno == ESRCH);
	errno = 0;
	UT_ASSERT(waitpid(child, &status, WNOHANG) == -1 && errno == ECHILD);
}

UT_CASE(reaper_sig_ign_autoreap_pid_reuse, 5000)
{
	struct sigaction old_action;
	struct sigaction action = {.sa_handler = SIG_IGN};
	pid_t first;
	pid_t second;
	int status;

	UT_ASSERT_EQ(sigemptyset(&action.sa_mask), 0);
	UT_ASSERT_EQ(sigaction(SIGCHLD, &action, &old_action), 0);

	first = UT_FORK();
	if (first == 0)
		_exit(0);
	/* SIG_IGN on SIGCHLD auto-reaps: no zombie, waitpid goes ECHILD. */
	UT_ASSERT(reaper_poll_until(reaper_echild, NULL));
	UT_ASSERT_EQ(sigaction(SIGCHLD, &old_action, NULL), 0);

	/* The freed pid is reused immediately (deterministic lowest-free). */
	second = UT_FORK();
	if (second == 0)
		_exit(0);
	UT_ASSERT_EQ(second, first);
	UT_EXPECT_EXIT(second, 0);
}

static volatile sig_atomic_t reaper_sigchld_count;
static struct {
	int code;
	int status;
	pid_t pid;
} reaper_sigchld_info;

static void reaper_sigchld_count_handler(int signal, siginfo_t *info,
					 void *context)
{
	(void)signal;
	(void)context;
	reaper_sigchld_info.code = info->si_code;
	reaper_sigchld_info.status = info->si_status;
	reaper_sigchld_info.pid = info->si_pid;
	reaper_sigchld_count++;
}

UT_CASE(reaper_sa_nocldwait_autoreap_echild, 5000)
{
	struct sigaction old_action;
	struct sigaction action = {
		.sa_sigaction = reaper_sigchld_count_handler,
		.sa_flags = SA_SIGINFO | SA_NOCLDWAIT,
	};
	pid_t first;
	pid_t second;
	int status;

	UT_ASSERT_EQ(sigemptyset(&action.sa_mask), 0);
	UT_ASSERT_EQ(sigaction(SIGCHLD, &action, &old_action), 0);
	reaper_sigchld_count = 0;

	first = UT_FORK();
	if (first == 0)
		_exit(5);
	UT_ASSERT(reaper_poll_until(reaper_echild, NULL));
	for (int tries = 0; tries < 200 && reaper_sigchld_count == 0;
	     tries++)
		reaper_sleep_ms(5);
	UT_ASSERT_EQ(reaper_sigchld_count, 1);
	UT_ASSERT_EQ(reaper_sigchld_info.code, CLD_EXITED);
	UT_ASSERT_EQ(reaper_sigchld_info.status, 5);
	UT_ASSERT_EQ(reaper_sigchld_info.pid, first);
	UT_ASSERT_EQ(sigaction(SIGCHLD, &old_action, NULL), 0);

	second = UT_FORK();
	if (second == 0)
		_exit(0);
	UT_ASSERT_EQ(second, first);
	UT_EXPECT_EXIT(second, 0);
}

UT_CASE(reaper_fork_exit_storm_wall, 10000)
{
	uint32_t seen = 0;
	int status;
	int reaped = 0;

	for (int i = 0; i < 24; i++) {
		pid_t child = UT_FORK();

		if (child == 0)
			_exit((i * 7) % 24);
	}

	/* Reap everything with the -1 selector and __WALL. */
	for (int tries = 0; tries < 2000 && reaped < 24; tries++) {
		pid_t child = waitpid(-1, &status, WNOHANG | __WALL);

		if (child == -1) {
			UT_ASSERT_ERRNO(waitpid(-1, &status, WNOHANG | __WALL),
					ECHILD);
			break;
		}
		if (child == 0) {
			reaper_sleep_ms(5);
			continue;
		}
		UT_ASSERT(WIFEXITED(status));
		seen |= 1U << (WEXITSTATUS(status) & 31);
		reaped++;
		errno = 0;
		UT_ASSERT(kill(child, 0) == -1 && errno == ESRCH);
	}
	UT_ASSERT_EQ(reaped, 24);
	UT_ASSERT_EQ(seen, 0xffffff);
}

UT_CASE(reaper_zombie_holds_pid_until_claimed, 5000)
{
	pid_t first;
	pid_t second;
	pid_t third;
	int status;

	first = UT_FORK();
	if (first == 0)
		_exit(0);
	UT_ASSERT(reaper_poll_until(reaper_zombie_visible,
				   (void *)(uintptr_t)first));

	/* second exits and is fully claimed: its pid is free again, but
	 * first is still a zombie holding its pid. */
	second = UT_FORK();
	if (second == 0)
		_exit(0);
	{
		struct reaper_claim claim = {.pid = second, .status = &status};

		UT_ASSERT(reaper_poll_until(reaper_claimable, &claim));
	}
	errno = 0;
	UT_ASSERT(kill(second, 0) == -1 && errno == ESRCH);
	UT_EXPECT_NE(second, first);

	/* Reuse happens only after first is claimed. */
	{
		struct reaper_claim claim = {.pid = first, .status = &status};

		UT_ASSERT(reaper_poll_until(reaper_claimable, &claim));
	}
	UT_ASSERT(kill(first, 0) == -1 && errno == ESRCH);
	third = UT_FORK();
	if (third == 0)
		_exit(0);
	UT_ASSERT_EQ(third, first);
	UT_EXPECT_EXIT(third, 0);
}
