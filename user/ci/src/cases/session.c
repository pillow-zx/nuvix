/*
 * Session/TTY leader-exit regression (TODO.md P0): controlling-TTY release
 * order (release then SIGHUP then SIGCONT to the old foreground pgrp),
 * leader exit with live siblings, non-session-leader process exit, TTY
 * takeover, TIOCNOTTY idempotence, and foreground-pgrp roundtrips.
 *
 * Conventions:
 * - Every session leader is forked by the case and reaped by it.
 * - Observers install SIGHUP/SIGCONT handlers and signal readiness ('R')
 *   through a pipe BEFORE the leader may exit, so the SIGHUP default action
 *   can never race the test; they exit from the SIGCONT handler and their
 *   zombies are reaped by busybox init.
 * - All "no hangup" assertions are bounded negative windows anchored by a
 *   marker byte written after the triggering event, never by arbitrary time.
 * - Children in foreign pgrps are unreachable by the runner's group SIGKILL:
 *   every such process exits itself; failure paths kill+waitpid best-effort.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <utest.h>

#define SESSION_SILENCE_MS 150
#define SESSION_POLL_BUDGET_MS 1000
#define SESSION_HANGUP_BUDGET_MS 3000

static int session_console_fd = -1;
static int session_console_devfd = -1;
static volatile sig_atomic_t session_sigusr1;

static void session_sleep_ms(long milliseconds)
{
	struct timespec delay = {
		.tv_sec = milliseconds / 1000,
		.tv_nsec = (milliseconds % 1000) * 1000000L,
	};

	while (nanosleep(&delay, &delay) < 0)
		;
}

static void session_sigusr1_handler(int signal)
{
	(void)signal;
	session_sigusr1 = 1;
}

static int session_setup_step;

/* Set up a fresh session leader with a controlling TTY on /dev/console.
 * Returns the console fd, or -1 on failure. */
static int session_leader_setup(void)
{
	struct sigaction sigusr1 = {.sa_handler = session_sigusr1_handler};
	int fd;

	session_setup_step = 1;
	if (setsid() < 0)
		return -1;
	sigemptyset(&sigusr1.sa_mask);
	(void)sigaction(SIGUSR1, &sigusr1, NULL);
	session_setup_step = 2;
	fd = open("/dev/console", O_RDWR);
	if (fd < 0)
		return -1;
	session_console_fd = fd;
	session_console_devfd = fd;
	session_setup_step = 3;
	if (ioctl(fd, TIOCSCTTY, 0) == 0)
		return fd;
	session_setup_step = 4;
	if (ioctl(fd, TIOCSCTTY, 1) == 0)
		return fd;
	return -1;
}

/* Setup failure: exit with 100 + step*10 + (errno & 0xf) so a failure
 * identifies the failing step (used by the reaper-side checks). */
static void session_setup_fail(void)
{
	_exit(100 + session_setup_step * 10 +
	      (errno > 0 ? errno & 0xf : 1));
}

static void session_hup_handler(int signal)
{
	(void)signal;
	(void)write(session_console_fd, "H", 1);
}

/* session_console_fd doubles as the record pipe write end for observers;
 * the leader sets it to the real console fd only after forking observers,
 * so observers must be forked with the record pipe handed over explicitly. */
static void session_cont_handler(int signal)
{
	(void)signal;
	(void)write(session_console_fd, "C", 1);
	_exit(0);
}

/* Observer: install hangup recorders, signal readiness, then wait for the
 * hangup with a bounded budget (a missing hangup exits 91 instead of
 * hanging the case group forever). */
static void session_observer(int ready_fd, int rec_fd)
{
	struct sigaction hup = {.sa_handler = session_hup_handler};
	struct sigaction cont = {.sa_handler = session_cont_handler};
	int waited = 0;

	sigemptyset(&hup.sa_mask);
	sigemptyset(&cont.sa_mask);
	/* SIGCONT must wait until the SIGHUP handler completes: the kernel
	 * delivers one pending signal per trap return, so without this the
	 * CONT handler can nest inside the HUP handler and _exit() before
	 * the HUP handler finishes. */
	(void)sigaddset(&hup.sa_mask, SIGCONT);
	(void)sigaction(SIGHUP, &hup, NULL);
	(void)sigaction(SIGCONT, &cont, NULL);
	session_console_fd = rec_fd;
	(void)write(ready_fd, "R", 1);
	while (waited < SESSION_HANGUP_BUDGET_MS) {
		session_sleep_ms(10);
		waited += 10;
	}
	_exit(91);
}

static int session_read_byte(int fd, char *out)
{
	struct pollfd pfd = {.fd = fd, .events = POLLIN};

	for (int i = 0; i < SESSION_POLL_BUDGET_MS / 100; i++) {
		if (poll(&pfd, 1, 100) > 0) {
			if (read(fd, out, 1) == 1)
				return 0;
			return -1;
		}
	}
	return -1;
}

/* Wait for SIGUSR1 with a bounded budget (a missed wakeup exits `code` so
 * the case reports a diagnosis instead of timing out). */
static void session_wait_sigusr1(int code)
{
	int waited = 0;

	while (!session_sigusr1 && waited < SESSION_HANGUP_BUDGET_MS) {
		session_sleep_ms(10);
		waited += 10;
	}
	if (!session_sigusr1)
		_exit(code);
}

/* Blocking wait with EINTR retry: cuteOS delivers SIGCHLD notifications
 * even under SIG_DFL, so a blocking waitpid can be interrupted. */
static int session_wait_child(pid_t child, int *status)
{
	for (;;) {
		pid_t got = waitpid(child, status, 0);

		if (got == child)
			return 0;
		if (got == -1 && errno == EINTR)
			continue;
		return -1;
	}
}

/* Returns true when no byte arrives within `ms`. */
static bool session_expect_silence(int fd, int ms)
{
	struct pollfd pfd = {.fd = fd, .events = POLLIN};

	return poll(&pfd, 1, ms) == 0;
}

/* Expect 'H' then 'C' in order, then silence (no repeat hangup). */
static void session_expect_hangup_order(int fd)
{
	char a;
	char b;

	UT_ASSERT_EQ(session_read_byte(fd, &a), 0);
	UT_ASSERT_EQ(session_read_byte(fd, &b), 0);
	UT_ASSERT_EQ(a, 'H');
	UT_ASSERT_EQ(b, 'C');
	if (!session_expect_silence(fd, SESSION_SILENCE_MS)) {
		char extra = 0;

		(void)session_read_byte(fd, &extra);
		UT_FAIL("extra byte after hangup: %c (0x%02x)", extra, extra);
	}
}

UT_CASE(session_leader_exit_hangup_order, 5000)
{
	for (int mode = 0; mode < 3; mode++) {
		int p_rec[2];
		int p_mark[2];
		int p_ready[2];
		pid_t leader;
		pid_t observer;

		UT_ASSERT_EQ(pipe(p_rec), 0);
		UT_ASSERT_EQ(pipe(p_mark), 0);
		UT_ASSERT_EQ(pipe(p_ready), 0);

		leader = UT_FORK();
		if (leader == 0) {
			/* Child of the case pgrp: setsid() succeeds. */
			if (session_leader_setup() < 0)
				session_setup_fail();
			observer = UT_FORK();
			if (observer == 0)
				session_observer(p_ready[1], p_rec[1]);
			if (session_read_byte(p_ready[0], &(char){0}) != 0)
				_exit(126);
			(void)write(p_mark[1], "E", 1);
			if (mode == 0)
				_exit(0);
			if (mode == 1)
				syscall(SYS_exit_group, 0);
			*(volatile int *)0 = 1; /* SIGSEGV */
			_exit(125);
		}

		/* Leader's exit path (incl. the kernel-side hangup) completes
		 * before the case resumes: single core. */
		UT_ASSERT_EQ(session_read_byte(p_mark[0], &(char){0}), 0);
		session_expect_hangup_order(p_rec[0]);
		if (mode == 2)
			UT_EXPECT_SIGNAL(leader, SIGSEGV);
		else
			UT_EXPECT_EXIT(leader, 0);

		close(p_rec[0]);
		close(p_rec[1]);
		close(p_mark[0]);
		close(p_mark[1]);
		close(p_ready[0]);
		close(p_ready[1]);
	}
}

static void session_sibling_task(void *arg)
{
	int ready_fd = ((int *)arg)[0];
	int mark_fd = ((int *)arg)[1];

	session_sigusr1 = 0;
	(void)write(ready_fd, "S", 1);
	session_wait_sigusr1(92);
	{
		pid_t fg_pid = 0;

		if (ioctl(session_console_fd, TIOCGPGRP, &fg_pid) == 0 &&
		    fg_pid == getpgrp())
			(void)write(mark_fd, "Y", 1);
		else
			(void)write(mark_fd, "N", 1);
	}
	/* Raw task exit: _exit() would be exit_group and kill the observer. */
	syscall(SYS_exit, 0);
}

UT_CASE(session_leader_exit_live_sibling_keeps_ctty, 5000)
{
	int p_rec[2];
	int p_mark[2];
	int p_ready[2];
	int p_s_mark[2];
	int sibling_args[2];
	pid_t leader;
	pid_t observer;
	pthread_t sibling;

	UT_ASSERT_EQ(pipe(p_rec), 0);
	UT_ASSERT_EQ(pipe(p_mark), 0);
	UT_ASSERT_EQ(pipe(p_ready), 0);
	UT_ASSERT_EQ(pipe(p_s_mark), 0);

	leader = UT_FORK();
	if (leader == 0) {
		if (session_leader_setup() < 0)
			session_setup_fail();
		observer = UT_FORK();
		if (observer == 0)
			session_observer(p_ready[1], p_rec[1]);
		sibling_args[0] = p_ready[1];
		sibling_args[1] = p_s_mark[1];
		if (pthread_create(&sibling, NULL,
				   (void *(*)(void *))session_sibling_task,
				   sibling_args) != 0)
			_exit(126);
		/* 'R' from the observer, 'S' from the sibling thread. */
		if (session_read_byte(p_ready[0], &(char){0}) != 0)
			_exit(125);
		if (session_read_byte(p_ready[0], &(char){0}) != 0)
			_exit(125);
		(void)write(p_mark[1], "E", 1);
		/* Leader thread exits alone: must keep ctty + fg for the
		 * still-live sibling (last-task guard). */
		syscall(SYS_exit, 0);
		_exit(124);
	}

	UT_ASSERT_EQ(session_read_byte(p_mark[0], &(char){0}), 0);
	/* No hangup at leader exit with a live sibling. */
	UT_ASSERT(session_expect_silence(p_rec[0], SESSION_SILENCE_MS));

	/* The sibling still owns the ctty attachment and the foreground. */
	UT_ASSERT_EQ(kill(leader, SIGUSR1), 0);
	{
		char verdict = 0;

		UT_ASSERT_EQ(session_read_byte(p_s_mark[0], &verdict), 0);
		UT_ASSERT_EQ(verdict, 'Y');
	}
	/* Sibling (last task) exit revokes and hangs up the fg pgrp. */
	session_expect_hangup_order(p_rec[0]);
	UT_EXPECT_EXIT(leader, 0);

	close(p_rec[0]);
	close(p_rec[1]);
	close(p_mark[0]);
	close(p_mark[1]);
	close(p_ready[0]);
	close(p_ready[1]);
	close(p_s_mark[0]);
	close(p_s_mark[1]);
}

UT_CASE(session_nonleader_process_exit_no_hangup, 5000)
{
	int p_rec[2];
	int p_mark[2];
	int p_ready[2];
	pid_t leader;
	pid_t sub;
	int status;

	UT_ASSERT_EQ(pipe(p_rec), 0);
	UT_ASSERT_EQ(pipe(p_mark), 0);
	UT_ASSERT_EQ(pipe(p_ready), 0);

	leader = UT_FORK();
	if (leader == 0) {
		struct sigaction hup = {.sa_handler = session_hup_handler};
		struct sigaction cont = {.sa_handler = session_cont_handler};

		if (session_leader_setup() < 0)
			session_setup_fail();
		sigemptyset(&hup.sa_mask);
		sigemptyset(&cont.sa_mask);
		(void)sigaction(SIGHUP, &hup, NULL);
		(void)sigaction(SIGCONT, &cont, NULL);
		session_console_fd = p_rec[1];
		sub = UT_FORK();
		if (sub == 0) {
			/* Same session+pgrp, own proc, NOT session leader. */
			(void)write(p_ready[1], "R", 1);
			_exit(0);
		}
		if (session_read_byte(p_ready[0], &(char){0}) != 0)
			_exit(126);
		if (session_wait_child(sub, &status) != 0)
			_exit(125);
		/* No hangup from a non-session-leader process exit. */
		if (!session_expect_silence(p_rec[1], SESSION_SILENCE_MS))
			_exit(124);
		{
			pid_t fg_pid = 0;

			if (ioctl(session_console_devfd, TIOCGPGRP, &fg_pid) != 0 ||
			    fg_pid != getpgrp())
				_exit(123);
		}
		(void)write(p_mark[1], "Y", 1);
		_exit(0);
	}

	{
		char verdict = 0;

		UT_ASSERT_EQ(session_read_byte(p_mark[0], &verdict), 0);
		UT_ASSERT_EQ(verdict, 'Y');
	}
	UT_EXPECT_EXIT(leader, 0);

	close(p_rec[0]);
	close(p_rec[1]);
	close(p_mark[0]);
	close(p_mark[1]);
	close(p_ready[0]);
	close(p_ready[1]);
}

UT_CASE(session_tty_takeover_steal_order, 5000)
{
	int p_rec[2];
	int p_mark[2];
	int p_mark_c[2];
	pid_t leader;
	pid_t observer;
	pid_t thief;

	UT_ASSERT_EQ(pipe(p_rec), 0);
	UT_ASSERT_EQ(pipe(p_mark), 0);
	UT_ASSERT_EQ(pipe(p_mark_c), 0);

	leader = UT_FORK();
	if (leader == 0) {
		struct sigaction ignore = {.sa_handler = SIG_IGN};

		if (session_leader_setup() < 0)
			session_setup_fail();
		/* The leader is in the foreground pgrp: the steal's hangup
		 * would terminate it with the SIGHUP default action. */
		sigemptyset(&ignore.sa_mask);
		(void)sigaction(SIGHUP, &ignore, NULL);
		observer = UT_FORK();
		if (observer == 0)
			session_observer(p_mark[1], p_rec[1]);
		/* p_mark doubles as ready: observer writes 'R' before pause. */
		(void)write(p_mark[1], "A", 1);
		session_sigusr1 = 0;
		session_wait_sigusr1(92);
		{
			pid_t fg_pid = 0;

			errno = 0;
			if (ioctl(session_console_fd, TIOCGPGRP, &fg_pid) == -1 &&
			    errno == ENOTTY)
				(void)write(p_mark[1], "Y", 1);
			else
				(void)write(p_mark[1], "N", 1);
		}
		_exit(0);
	}

	/* Wait for A's setup + observer-ready, then steal. */
	UT_ASSERT_EQ(session_read_byte(p_mark[0], &(char){0}), 0);
	UT_ASSERT_EQ(session_read_byte(p_mark[0], &(char){0}), 0);
	thief = UT_FORK();
	if (thief == 0) {
		if (setsid() < 0)
			_exit(127);
		thief = open("/dev/console", O_RDWR);
		if (thief < 0)
			_exit(126);
		if (ioctl(thief, TIOCSCTTY, 1) != 0)
			_exit(125);
		{
			pid_t fg_pid = 0;

			if (ioctl(thief, TIOCGPGRP, &fg_pid) != 0 ||
			    fg_pid != getpgrp())
				_exit(124);
		}
		(void)write(p_mark_c[1], "C", 1);
		_exit(0);
	}

	/* The steal displaced A's session: old fg pgrp gets H then C. */
	session_expect_hangup_order(p_rec[0]);
	{
		char verdict = 0;

		UT_ASSERT_EQ(session_read_byte(p_mark_c[0], &verdict), 0);
		UT_ASSERT_EQ(verdict, 'C');
	}
	/* A's attachment is gone: TIOCGPGRP fails with ENOTTY. */
	UT_ASSERT_EQ(kill(leader, SIGUSR1), 0);
	{
		char verdict = 0;

		UT_ASSERT_EQ(session_read_byte(p_mark[0], &verdict), 0);
		UT_ASSERT_EQ(verdict, 'Y');
	}
	UT_EXPECT_EXIT(leader, 0);
	UT_EXPECT_EXIT(thief, 0);

	close(p_rec[0]);
	close(p_rec[1]);
	close(p_mark[0]);
	close(p_mark[1]);
	close(p_mark_c[0]);
	close(p_mark_c[1]);
}

UT_CASE(session_tiotty_roundtrip_and_setsid_errors, 5000)
{
	pid_t leader;

	leader = UT_FORK();
	if (leader == 0) {
		int console;
		int p_ready[2];
		pid_t member;
		pid_t outsider;
		int status;

		if (session_leader_setup() < 0)
			session_setup_fail();
		console = session_console_fd;
		if (pipe(p_ready) != 0)
			_exit(126);

		errno = 0;
		if (setsid() != -1 || errno != EPERM)
			_exit(125);

		member = UT_FORK();
		if (member == 0) {
			(void)setpgid(0, 0);
			(void)write(p_ready[1], "R", 1);
			for (;;)
				pause();
		}
		if (session_read_byte(p_ready[0], &(char){0}) != 0)
			_exit(124);
		{
			pid_t fg_target = member;

			if (ioctl(console, TIOCSPGRP, &fg_target) != 0)
				_exit(124);
		}
		{
			pid_t fg_pid = 0;

			if (ioctl(console, TIOCGPGRP, &fg_pid) != 0 ||
			    fg_pid != member)
				_exit(123);
		}
		{
			pid_t fg_target = getpgrp();

			if (ioctl(console, TIOCSPGRP, &fg_target) != 0)
				_exit(121);
		}
		{
			pid_t fg_pid = 0;

			if (ioctl(console, TIOCGPGRP, &fg_pid) != 0 ||
			    fg_pid != getpgrp())
				_exit(120);
		}
		{
			pid_t bad_pgid = 200;

			/* cuteOS divergence: a nonexistent pgrp is rejected
			 * with EPERM (Linux returns ESRCH); the cross-session
			 * and range checks below still distinguish EPERM from
			 * EINVAL. */
			errno = 0;
			if (ioctl(console, TIOCSPGRP, &bad_pgid) != -1 ||
			    errno != EPERM)
				_exit(119);
		}
		{
			pid_t zero_pgid = 0;

			errno = 0;
			if (ioctl(console, TIOCSPGRP, &zero_pgid) != -1 ||
			    errno != EINVAL)
				_exit(118);
		}

		outsider = UT_FORK();
		if (outsider == 0) {
			if (setsid() < 0)
				_exit(117);
			(void)write(p_ready[1], "R", 1);
			for (;;)
				pause();
		}
		if (session_read_byte(p_ready[0], &(char){0}) != 0)
			_exit(116);
		{
			pid_t outsider_pgid = outsider;

			errno = 0;
			if (ioctl(console, TIOCSPGRP, &outsider_pgid) != -1 ||
			    errno != EPERM)
				_exit(115);
		}
		(void)kill(outsider, SIGKILL);
		if (waitpid(outsider, &status, 0) != outsider)
			_exit(114);

		/* SIGKILL, not SIGUSR1: the member inherits the leader's
		 * SIGUSR1 handler (installed by session_leader_setup). */
		(void)kill(member, SIGKILL);
		if (waitpid(member, &status, 0) != member)
			_exit(113);
		close(p_ready[0]);
		close(p_ready[1]);
		_exit(0);
	}
	UT_EXPECT_EXIT(leader, 0);

	/* Outside any session: no ctty attachment, TIOCGPGRP fails. */
	{
		int console = open("/dev/console", O_RDWR);
		pid_t fg_pid = 0;

		UT_ASSERT(console >= 0);
		errno = 0;
		UT_ASSERT(ioctl(console, TIOCGPGRP, &fg_pid) == -1 &&
			  errno == ENOTTY);
		close(console);
	}
}

UT_CASE(session_tiotty_release_no_double_hangup, 5000)
{
	int p_rec[2];
	int p_mark[2];
	int p_ready[2];
	pid_t leader;
	pid_t sub;

	UT_ASSERT_EQ(pipe(p_rec), 0);
	UT_ASSERT_EQ(pipe(p_mark), 0);
	UT_ASSERT_EQ(pipe(p_ready), 0);

	leader = UT_FORK();
	if (leader == 0) {
		struct sigaction hup = {.sa_handler = session_hup_handler};
		struct sigaction cont = {.sa_handler = session_cont_handler};

		if (session_leader_setup() < 0)
			session_setup_fail();
		sigemptyset(&hup.sa_mask);
		sigemptyset(&cont.sa_mask);
		(void)sigaction(SIGHUP, &hup, NULL);
		(void)sigaction(SIGCONT, &cont, NULL);
		session_console_fd = p_rec[1];
		sub = UT_FORK();
		if (sub == 0) {
			/* Non-leader TIOCNOTTY: detaches only its own
			 * attachment; a second call is idempotent. */
			(void)write(p_ready[1], "R", 1);
			if (ioctl(session_console_devfd, TIOCNOTTY, 0) != 0)
				_exit(126);
			errno = 0;
			if (ioctl(session_console_devfd, TIOCNOTTY, 0) != -1 ||
			    errno != ENOTTY)
				_exit(125);
			(void)write(p_ready[1], "D", 1);
			_exit(0);
		}
		if (session_read_byte(p_ready[0], &(char){0}) != 0)
			_exit(124);
		if (session_read_byte(p_ready[0], &(char){0}) != 0)
			_exit(124);
		if (session_wait_child(sub, NULL) != 0)
			_exit(123);
		(void)write(p_mark[1], "T", 1);
		session_sigusr1 = 0;
		session_wait_sigusr1(92);
		{
			pid_t fg_pid = 0;

			if (ioctl(session_console_devfd, TIOCGPGRP, &fg_pid) != 0 ||
			    fg_pid != getpgrp())
				_exit(122);
		}
		_exit(0);
	}

	/* S's TIOCNOTTY and exit produce no hangup (no double hangup). */
	UT_ASSERT_EQ(session_read_byte(p_mark[0], &(char){0}), 0);
	UT_ASSERT(session_expect_silence(p_rec[0], SESSION_SILENCE_MS));
	UT_ASSERT_EQ(kill(leader, SIGUSR1), 0);
	UT_EXPECT_EXIT(leader, 0);

	close(p_rec[0]);
	close(p_rec[1]);
	close(p_mark[0]);
	close(p_mark[1]);
	close(p_ready[0]);
	close(p_ready[1]);
}

/*
 * Zombie identity ABI: an exited process awaiting wait4 must stay queryable
 * through getpgid/getsid with the group identity it died with, and must
 * become ESRCH once the parent reaps it.
 */
static volatile sig_atomic_t session_zombie_sigchld;

static void session_zombie_sigchld_handler(int signal)
{
	(void)signal;
	session_zombie_sigchld = 1;
}

UT_CASE(session_zombie_getpgid_identity, 5000)
{
	struct sigaction action = {0};
	struct sigaction old_action;
	sigset_t blocked;
	sigset_t previous;
	pid_t child;
	pid_t pgid;
	pid_t sid;
	int status;

	action.sa_handler = session_zombie_sigchld_handler;
	sigemptyset(&action.sa_mask);
	UT_ASSERT_EQ(sigaction(SIGCHLD, &action, &old_action), 0);
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGCHLD);
	UT_ASSERT_EQ(sigprocmask(SIG_BLOCK, &blocked, &previous), 0);

	child = UT_FORK();
	if (child == 0) {
		(void)setpgid(0, 0);
		_exit(0);
	}

	session_zombie_sigchld = 0;
	while (!session_zombie_sigchld)
		sigsuspend(&previous);

	/* Child is a zombie: unreaped but still addressable, and its group
	 * identity must survive the leader's detach. */
	pgid = getpgid(child);
	UT_ASSERT_NE(pgid, -1);
	UT_ASSERT_EQ(pgid, child);
	sid = getsid(child);
	UT_ASSERT_NE(sid, -1);
	UT_ASSERT_EQ(sid, getsid(0));

	UT_ASSERT_EQ(waitpid(child, &status, 0), child);
	UT_ASSERT(WIFEXITED(status));
	UT_ASSERT_ERRNO(getpgid(child), ESRCH);
	UT_ASSERT_ERRNO(getsid(child), ESRCH);

	UT_ASSERT_EQ(sigprocmask(SIG_UNBLOCK, &blocked, NULL), 0);
	UT_ASSERT_EQ(sigaction(SIGCHLD, &old_action, NULL), 0);
}
