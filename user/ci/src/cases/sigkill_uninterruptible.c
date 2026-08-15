/* sigkill_uninterruptible.c - SIGKILL must not wake a task out of a
 * TASK_WAIT_UNINTERRUPTIBLE wait.  The kernel mutex and exec-serialization
 * waits treat only WAIT_OUTCOME_EVENT as a valid wake; waking them with a
 * signal would resume the task with WAIT_OUTCOME_SIGNAL and trip their
 * BUG_ON.  The signal stays pending and is delivered when the task later
 * returns to user mode.
 *
 * Trigger: child A blocks in a syslog read holding the printk read lock
 * (interruptible wait); child B blocks on that mutex (uninterruptible
 * wait); SIGKILL to B must leave it asleep, not panic the kernel. */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <utest.h>

enum {
	SYSLOG_ACTION_READ = 2,
	SYSLOG_ACTION_READ_ALL = 3,
	SYSLOG_ACTION_CLEAR = 5,
	SYSLOG_ACTION_SIZE_UNREAD = 9,
};

static long log_klogctl(int type, char *buffer, int size)
{
	return syscall(SYS_syslog, type, buffer, size);
}

static void ut_sleep_ms(long ms)
{
	(void)nanosleep(&(struct timespec){.tv_sec = ms / 1000,
					   .tv_nsec = (ms % 1000) * 1000000L},
			NULL);
}

static void sigusr1_handler(int signal)
{
	(void)signal;
}

/* CLEAR (syslog action 5) empties only the snapshot view (clear_seq); the
 * destructive read cursor (read_seq) is a separate, independent cursor,
 * exactly as in the Linux syslog(2) ABI (CLEAR has no effect on READ or
 * SIZE_UNREAD).  Drain the read cursor so a subsequent syslog READ blocks
 * on a genuinely empty ring. */
static void log_drain_ring(char *buffer, size_t size)
{
	for (;;) {
		long unread = log_klogctl(SYSLOG_ACTION_SIZE_UNREAD, NULL, 0);
		long read;

		if (unread <= 0)
			break;
		read = log_klogctl(SYSLOG_ACTION_READ, buffer, (int)size);
		if (read <= 0)
			break;
	}
}

UT_CASE(sigkill_does_not_wake_uninterruptible_wait, 15000)
{
	struct sigaction action = {.sa_handler = sigusr1_handler};
	struct sigaction old_action;
	char buffer[64];
	pid_t a;
	pid_t b;
	int status;

	UT_ASSERT_EQ(log_klogctl(SYSLOG_ACTION_CLEAR, NULL, 0), 0);
	log_drain_ring(buffer, sizeof(buffer));
	UT_ASSERT_EQ(log_klogctl(SYSLOG_ACTION_READ_ALL, buffer,
				 sizeof(buffer)),
		     0);
	UT_ASSERT_EQ(log_klogctl(SYSLOG_ACTION_SIZE_UNREAD, NULL, 0), 0);
	UT_ASSERT_EQ(sigemptyset(&action.sa_mask), 0);
	UT_ASSERT_EQ(sigaction(SIGUSR1, &action, &old_action), 0);

	/* A blocks in the interruptible syslog read, holding the printk read
	 * lock across the wait. */
	a = UT_FORK();
	if (a == 0) {
		(void)log_klogctl(SYSLOG_ACTION_READ, buffer, sizeof(buffer));
		_exit(0);
	}
	ut_sleep_ms(100);

	/* B blocks on the printk read mutex: a TASK_WAIT_UNINTERRUPTIBLE
	 * wait.  SIGKILL must leave it asleep. */
	b = UT_FORK();
	if (b == 0) {
		(void)log_klogctl(SYSLOG_ACTION_READ, buffer, sizeof(buffer));
		_exit(0);
	}
	ut_sleep_ms(100);

	errno = 0;
	if (kill(b, SIGKILL) != 0)
		UT_FAIL("kill(B, SIGKILL) failed: errno=%d", errno);
	ut_sleep_ms(100);

	/* Wake A out of the syslog read so it releases the lock.  B then
	 * acquires it and re-blocks in the interruptible syslog wait, where
	 * the pending, unblockable SIGKILL self-wakes it (interruptible waits
	 * abort on a pending signal); B returns -EINTR and is killed at the
	 * trap-return boundary. */
	UT_ASSERT_EQ(kill(a, SIGUSR1), 0);

	UT_ASSERT_EQ(waitpid(b, &status, 0), b);
	UT_ASSERT(WIFSIGNALED(status));
	UT_ASSERT_EQ(WTERMSIG(status), SIGKILL);

	UT_ASSERT_EQ(waitpid(a, &status, 0), a);
	UT_ASSERT(WIFEXITED(status));
	UT_ASSERT_EQ(WEXITSTATUS(status), 0);

	UT_ASSERT_EQ(sigaction(SIGUSR1, &old_action, NULL), 0);
}
