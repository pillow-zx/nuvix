#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <utest.h>

static volatile sig_atomic_t signal_count;
static volatile sig_atomic_t signal_child_count;
static char *signal_altstack_base;
static size_t signal_altstack_size;

enum {
	RISCV_UCONTEXT_A0 = 10,
};

static const uintptr_t signal_restored_a0 = 0x1234567800000000UL;

static void signal_count_handler(int signal)
{
	(void)signal;
	signal_count++;
}

static void signal_altstack_handler(int signal)
{
	char marker;

	(void)signal;
	if (&marker >= signal_altstack_base &&
	    &marker < signal_altstack_base + signal_altstack_size)
		signal_count++;
}

static void signal_child_handler(int signal)
{
	(void)signal;
	signal_child_count++;
}

static void signal_restore_a0_handler(int signal, siginfo_t *info,
				      void *context)
{
	ucontext_t *ucontext = context;

	(void)signal;
	(void)info;
	ucontext->uc_mcontext.__gregs[RISCV_UCONTEXT_A0] = signal_restored_a0;
}

static void signal_sleep_ms(long milliseconds)
{
	struct timespec delay = {
		.tv_sec = milliseconds / 1000,
		.tv_nsec = (milliseconds % 1000) * 1000000L,
	};

	while (nanosleep(&delay, &delay) < 0)
		;
}

static void signal_burn_cpu(unsigned int milliseconds)
{
	struct timespec start;
	struct timespec now;
	uint64_t elapsed;
	volatile uint64_t value = 1;

	if (clock_gettime(CLOCK_MONOTONIC, &start) < 0)
		_exit(125);
	for (;;) {
		for (unsigned int index = 0; index < 10000; index++)
			value = value * 6364136223846793005ULL + 1;
		if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
			_exit(125);
		elapsed = (uint64_t)(now.tv_sec - start.tv_sec) * 1000000000ULL;
		if (now.tv_nsec >= start.tv_nsec)
			elapsed += (uint64_t)(now.tv_nsec - start.tv_nsec);
		else
			elapsed -= (uint64_t)(start.tv_nsec - now.tv_nsec);
		if (elapsed >= (uint64_t)milliseconds * 1000000ULL)
			break;
	}
	if (value == 0)
		_exit(125);
}

static clock_t signal_timeval_ticks(const struct timeval *timeval)
{
	return (clock_t)(timeval->tv_sec * 100L + timeval->tv_usec / 10000L);
}

UT_CASE(signal_handler_mask_pending_and_altstack, 1500)
{
	struct sigaction action = {
		.sa_handler = signal_count_handler,
	};
	struct sigaction old_action;
	sigset_t block;
	sigset_t pending;
	stack_t stack;
	stack_t old_stack;
	char alternate_stack[SIGSTKSZ];

	UT_ASSERT_EQ(sigemptyset(&action.sa_mask), 0);
	UT_ASSERT_EQ(sigaction(SIGUSR1, &action, &old_action), 0);
	UT_ASSERT_EQ(sigemptyset(&block), 0);
	UT_ASSERT_EQ(sigaddset(&block, SIGUSR1), 0);
	signal_count = 0;
	UT_ASSERT_EQ(sigprocmask(SIG_BLOCK, &block, NULL), 0);
	UT_ASSERT_EQ(kill(getpid(), SIGUSR1), 0);
	UT_ASSERT_EQ(sigpending(&pending), 0);
	UT_EXPECT(sigismember(&pending, SIGUSR1));
	UT_EXPECT_ERRNO(syscall(SYS_rt_sigpending, NULL, sizeof(pending)),
			EINVAL);
	UT_EXPECT_ERRNO(syscall(SYS_rt_sigpending, NULL, sizeof(unsigned long)),
			EFAULT);
	UT_ASSERT_EQ(sigpending(&pending), 0);
	UT_EXPECT(sigismember(&pending, SIGUSR1));
	UT_ASSERT_EQ(sigprocmask(SIG_UNBLOCK, &block, NULL), 0);
	UT_EXPECT_EQ(signal_count, 1);
	stack = (stack_t){
		.ss_sp = alternate_stack,
		.ss_size = sizeof(alternate_stack),
	};
	signal_altstack_base = alternate_stack;
	signal_altstack_size = sizeof(alternate_stack);
	UT_ASSERT_EQ(sigaltstack(&stack, &old_stack), 0);
	action.sa_handler = signal_altstack_handler;
	action.sa_flags = SA_ONSTACK;
	UT_ASSERT_EQ(sigaction(SIGUSR1, &action, NULL), 0);
	signal_count = 0;
	UT_ASSERT_EQ(kill(getpid(), SIGUSR1), 0);
	UT_EXPECT_EQ(signal_count, 1);
	UT_ASSERT_EQ(sigaltstack(&old_stack, NULL), 0);
	UT_ASSERT_EQ(sigaction(SIGUSR1, &old_action, NULL), 0);
}

UT_CASE(signal_altstack_address_overflow, 1500)
{
	struct sigaction action = {
		.sa_handler = signal_count_handler,
		.sa_flags = SA_ONSTACK,
	};
	stack_t stack = {
		.ss_sp = (void *)(UINTPTR_MAX - 0x7fff),
		.ss_size = 64 * 1024,
	};
	pid_t child;

	child = UT_FORK();
	if (child == 0) {
		void *mapping;

		mapping = mmap((void *)0x7000, 4096, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
		if (mapping != (void *)0x7000)
			_exit(127);
		if (sigaltstack(&stack, NULL) < 0)
			_exit(126);
		if (sigaction(SIGUSR1, &action, NULL) < 0)
			_exit(125);
		(void)kill(getpid(), SIGUSR1);
		_exit(124);
	}
	UT_EXPECT_SIGNAL(child, SIGSEGV);
}

UT_CASE(signal_rt_sigsuspend_abi_and_mask_restore, 5000)
{
	struct sigaction action = {
		.sa_handler = signal_count_handler,
		.sa_flags = SA_RESTART,
	};
	struct sigaction old_action;
	sigset_t blocked;
	sigset_t suspend_mask;
	sigset_t observed;
	sigset_t old_mask;
	pid_t child;

	UT_ASSERT_EQ(sigemptyset(&suspend_mask), 0);
	UT_EXPECT_ERRNO(
		syscall(SYS_rt_sigsuspend, &suspend_mask, sizeof(suspend_mask)),
		EINVAL);
	UT_EXPECT_ERRNO(syscall(SYS_rt_sigsuspend, NULL, sizeof(unsigned long)),
			EFAULT);
	UT_EXPECT_ERRNO(syscall(SYS_rt_sigsuspend, &suspend_mask,
				sizeof(unsigned long) - 1),
			EINVAL);
	UT_ASSERT_EQ(sigemptyset(&action.sa_mask), 0);
	UT_ASSERT_EQ(sigaction(SIGUSR1, &action, &old_action), 0);
	UT_ASSERT_EQ(sigemptyset(&blocked), 0);
	UT_ASSERT_EQ(sigaddset(&blocked, SIGUSR1), 0);
	UT_ASSERT_EQ(sigprocmask(SIG_BLOCK, &blocked, &old_mask), 0);
	UT_ASSERT_EQ(sigemptyset(&suspend_mask), 0);
	UT_ASSERT_EQ(sigaddset(&suspend_mask, SIGCHLD), 0);
	signal_count = 0;
	child = UT_FORK();
	if (child == 0) {
		signal_sleep_ms(30);
		(void)kill(getppid(), SIGUSR1);
		_exit(0);
	}
	UT_EXPECT_ERRNO(sigsuspend(&suspend_mask), EINTR);
	UT_EXPECT_EQ(signal_count, 1);
	UT_ASSERT_EQ(sigprocmask(SIG_SETMASK, NULL, &observed), 0);
	UT_EXPECT(sigismember(&observed, SIGUSR1));
	UT_EXPECT_EXIT(child, 0);
	UT_ASSERT_EQ(sigprocmask(SIG_SETMASK, &old_mask, NULL), 0);
	UT_ASSERT_EQ(sigaction(SIGUSR1, &old_action, NULL), 0);
}

UT_CASE(signal_sigchld_process_group_and_fatal_status, 5000)
{
	struct sigaction action = {
		.sa_handler = signal_child_handler,
	};
	struct sigaction old_action;
	int report_pipe[2];
	pid_t child;

	UT_ASSERT_EQ(sigemptyset(&action.sa_mask), 0);
	UT_ASSERT_EQ(sigaction(SIGCHLD, &action, &old_action), 0);
	signal_child_count = 0;
	child = UT_FORK();
	if (child == 0)
		_exit(0);
	UT_EXPECT_EXIT(child, 0);
	UT_EXPECT(signal_child_count > 0);
	UT_ASSERT_EQ(sigaction(SIGCHLD, &old_action, NULL), 0);
	UT_ASSERT_EQ(pipe(report_pipe), 0);
	action.sa_handler = signal_count_handler;
	UT_ASSERT_EQ(sigaction(SIGUSR2, &action, &old_action), 0);
	signal_count = 0;
	child = UT_FORK();
	if (child == 0) {
		char report = 0;

		(void)close(report_pipe[0]);
		while (!signal_count)
			pause();
		report = 'g';
		(void)write(report_pipe[1], &report, 1);
		_exit(0);
	}
	UT_ASSERT_EQ(close(report_pipe[1]), 0);
	signal_sleep_ms(20);
	UT_ASSERT_EQ(kill(-getpgrp(), SIGUSR2), 0);
	UT_EXPECT_EQ(signal_count, 1);
	UT_EXPECT_EQ(read(report_pipe[0], &(char){0}, 1), 1);
	UT_EXPECT_EXIT(child, 0);
	UT_ASSERT_EQ(close(report_pipe[0]), 0);
	UT_ASSERT_EQ(sigaction(SIGUSR2, &old_action, NULL), 0);
	child = UT_FORK();
	if (child == 0) {
		volatile int *invalid = (volatile int *)0;

		*invalid = 1;
		_exit(127);
	}
	UT_EXPECT_SIGNAL(child, SIGSEGV);
}

UT_CASE(signal_eintr_restart_and_sigreturn_restore, 5000)
{
	struct sigaction action = {
		.sa_handler = signal_count_handler,
	};
	struct sigaction old_action;
	int pipefd[2];
	pid_t child;
	char byte;

	UT_ASSERT_EQ(sigemptyset(&action.sa_mask), 0);
	UT_ASSERT_EQ(sigaction(SIGUSR1, &action, &old_action), 0);
	UT_ASSERT_EQ(pipe(pipefd), 0);
	child = UT_FORK();
	if (child == 0) {
		signal_sleep_ms(30);
		(void)kill(getppid(), SIGUSR1);
		_exit(0);
	}
	UT_EXPECT_ERRNO(read(pipefd[0], &byte, 1), EINTR);
	UT_EXPECT_EXIT(child, 0);
	action = (struct sigaction){
		.sa_sigaction = signal_restore_a0_handler,
		.sa_flags = SA_SIGINFO,
	};
	UT_ASSERT_EQ(sigemptyset(&action.sa_mask), 0);
	UT_ASSERT_EQ(sigaction(SIGUSR1, &action, NULL), 0);
	child = UT_FORK();
	if (child == 0) {
		signal_sleep_ms(30);
		(void)kill(getppid(), SIGUSR1);
		_exit(0);
	}
	UT_EXPECT_EQ((uintptr_t)read(pipefd[0], &byte, 1), signal_restored_a0);
	UT_EXPECT_EXIT(child, 0);
	action = (struct sigaction){
		.sa_handler = signal_count_handler,
		.sa_flags = SA_RESTART,
	};
	UT_ASSERT_EQ(sigemptyset(&action.sa_mask), 0);
	UT_ASSERT_EQ(sigaction(SIGUSR1, &action, NULL), 0);
	child = UT_FORK();
	if (child == 0) {
		signal_sleep_ms(30);
		(void)kill(getppid(), SIGUSR1);
		signal_sleep_ms(30);
		(void)write(pipefd[1], "r", 1);
		_exit(0);
	}
	UT_ASSERT_EQ(read(pipefd[0], &byte, 1), 1);
	UT_EXPECT_EQ(byte, 'r');
	UT_EXPECT_EXIT(child, 0);
	UT_ASSERT_EQ(close(pipefd[0]), 0);
	UT_ASSERT_EQ(close(pipefd[1]), 0);
	UT_ASSERT_EQ(sigaction(SIGUSR1, &old_action, NULL), 0);
}

static struct {
	int code;
	int status;
	pid_t pid;
	uid_t uid;
	clock_t utime;
	clock_t stime;
} signal_cld_info;

static void signal_cld_handler(int signal, siginfo_t *info, void *context)
{
	(void)signal;
	(void)context;
	signal_cld_info.code = info->si_code;
	signal_cld_info.status = info->si_status;
	signal_cld_info.pid = info->si_pid;
	signal_cld_info.uid = info->si_uid;
	signal_cld_info.utime = info->si_utime;
	signal_cld_info.stime = info->si_stime;
	signal_child_count++;
}

/* Poll until the SA_SIGINFO SIGCHLD handler captured the expected event
 * (delivery happens on the parent's next user return), then verify it. */
static void signal_expect_cld(int want_code, int want_status)
{
	int tries;

	for (tries = 0; tries < 200 && signal_child_count == 0; tries++)
		signal_sleep_ms(5);
	UT_ASSERT(signal_child_count > 0);
	UT_ASSERT_EQ(signal_cld_info.code, want_code);
	UT_ASSERT_EQ(signal_cld_info.status, want_status);
	UT_ASSERT(signal_cld_info.pid > 0);
	UT_ASSERT(signal_cld_info.utime >= 0);
	UT_ASSERT(signal_cld_info.stime >= 0);
	signal_child_count = 0;
}

static void signal_expect_cld_for_child(int want_code, int want_status,
					pid_t child)
{
	signal_expect_cld(want_code, want_status);
	UT_ASSERT_EQ(signal_cld_info.pid, child);
}

static void *signal_thread_group_worker(void *argument)
{
	int fd = *(int *)argument;

	signal_burn_cpu(140);
	if (write(fd, "T", 1) != 1)
		_exit(125);
	return NULL;
}

UT_CASE(signal_sigchld_nocldstop_stop_continue_and_exit, 5000)
{
	struct sigaction action = {
		.sa_handler = signal_child_handler,
		.sa_flags = SA_NOCLDSTOP,
	};
	struct sigaction old_action;
	pid_t child;
	int status;

	UT_ASSERT_EQ(sigemptyset(&action.sa_mask), 0);
	UT_ASSERT_EQ(sigaction(SIGCHLD, &action, &old_action), 0);
	signal_child_count = 0;

	child = UT_FORK();
	if (child == 0) {
		(void)raise(SIGSTOP);
		for (;;)
			pause();
	}

	/* The stop event stays waitable under SA_NOCLDSTOP. */
	UT_ASSERT_EQ(waitpid(child, &status, WUNTRACED), child);
	UT_ASSERT(WIFSTOPPED(status));
	UT_ASSERT_EQ(WSTOPSIG(status), SIGSTOP);
	signal_sleep_ms(30);
	UT_ASSERT_EQ(signal_child_count, 0);

	UT_ASSERT_EQ(kill(child, SIGCONT), 0);
	UT_ASSERT_EQ(waitpid(child, &status, WCONTINUED), child);
	UT_ASSERT(WIFCONTINUED(status));
	signal_sleep_ms(30);
	UT_ASSERT_EQ(signal_child_count, 0);

	/* The exit notification is NOT suppressed by SA_NOCLDSTOP. */
	UT_ASSERT_EQ(kill(child, SIGKILL), 0);
	signal_sleep_ms(30);
	UT_ASSERT_EQ(signal_child_count, 1);
	UT_EXPECT_SIGNAL(child, SIGKILL);

	UT_ASSERT_EQ(sigaction(SIGCHLD, &old_action, NULL), 0);
}

UT_CASE(signal_sigchld_nocldwait_autoreap_echild, 5000)
{
	struct sigaction action = {
		.sa_sigaction = signal_cld_handler,
		.sa_flags = SA_SIGINFO | SA_NOCLDWAIT,
	};
	struct sigaction old_action;
	pid_t child;
	int status;
	int tries;

	UT_ASSERT_EQ(sigemptyset(&action.sa_mask), 0);
	UT_ASSERT_EQ(sigaction(SIGCHLD, &action, &old_action), 0);
	signal_child_count = 0;

	child = UT_FORK();
	if (child == 0)
		_exit(5);
	for (tries = 0; tries < 200; tries++) {
		errno = 0;
		if (waitpid(child, &status, WNOHANG) == -1 &&
		    errno == ECHILD)
			break;
		signal_sleep_ms(5);
	}
	UT_ASSERT(tries < 200);
	signal_expect_cld_for_child(CLD_EXITED, 5, child);
	UT_ASSERT_EQ(signal_cld_info.uid, 0);

	UT_ASSERT_EQ(sigaction(SIGCHLD, &old_action, NULL), 0);
}

UT_CASE(signal_sigchld_cld_siginfo_events, 5000)
{
	struct sigaction action = {
		.sa_sigaction = signal_cld_handler,
		.sa_flags = SA_SIGINFO,
	};
	struct sigaction old_action;
	pid_t child;
	int status;
	struct rusage usage;
	clock_t event_utime;
	clock_t event_stime;

	UT_ASSERT_EQ(sigemptyset(&action.sa_mask), 0);
	UT_ASSERT_EQ(sigaction(SIGCHLD, &action, &old_action), 0);
	signal_child_count = 0;

	/* CLD_EXITED with the exit code in si_status. */
	child = UT_FORK();
	if (child == 0) {
		signal_burn_cpu(150);
		_exit(42);
	}
	signal_expect_cld(CLD_EXITED, 42);
	UT_ASSERT_EQ(signal_cld_info.uid, 0);
	event_utime = signal_cld_info.utime;
	event_stime = signal_cld_info.stime;
	UT_ASSERT(event_utime + event_stime > 0);
	memset(&usage, 0, sizeof(usage));
	UT_ASSERT_EQ(syscall(SYS_wait4, child, &status, 0, &usage), child);
	UT_ASSERT(WIFEXITED(status));
	UT_ASSERT_EQ(WEXITSTATUS(status), 42);
	UT_ASSERT_EQ(event_utime, signal_timeval_ticks(&usage.ru_utime));
	UT_ASSERT_EQ(event_stime, signal_timeval_ticks(&usage.ru_stime));

	/* CLD_KILLED with the terminating signal. */
	child = UT_FORK();
	if (child == 0)
		for (;;)
			pause();
	UT_ASSERT_EQ(kill(child, SIGKILL), 0);
	signal_expect_cld(CLD_KILLED, SIGKILL);
	UT_EXPECT_SIGNAL(child, SIGKILL);

	/* A SIGSEGV death reports CLD_KILLED (cuteOS has no core model:
	 * the 0x80 coredump bit is never set, so no CLD_DUMPED). */
	child = UT_FORK();
	if (child == 0) {
		volatile int *invalid = (volatile int *)0;

		*invalid = 1;
		_exit(127);
	}
	signal_expect_cld(CLD_KILLED, SIGSEGV);
	UT_EXPECT_SIGNAL(child, SIGSEGV);

	/* Stop and continue events. */
	child = UT_FORK();
	if (child == 0) {
		signal_burn_cpu(120);
		(void)raise(SIGSTOP);
		for (;;)
			pause();
	}
	signal_expect_cld(CLD_STOPPED, SIGSTOP);
	UT_ASSERT(signal_cld_info.utime + signal_cld_info.stime > 0);
	event_utime = signal_cld_info.utime;
	event_stime = signal_cld_info.stime;
	UT_ASSERT_EQ(waitpid(child, &status, WUNTRACED), child);
	UT_ASSERT(WIFSTOPPED(status));
	UT_ASSERT_EQ(WSTOPSIG(status), SIGSTOP);
	UT_ASSERT_EQ(kill(child, SIGCONT), 0);
	signal_expect_cld(CLD_CONTINUED, SIGCONT);
	UT_ASSERT(signal_cld_info.utime >= event_utime);
	UT_ASSERT(signal_cld_info.stime >= event_stime);
	event_utime = signal_cld_info.utime;
	event_stime = signal_cld_info.stime;
	UT_ASSERT_EQ(waitpid(child, &status, WCONTINUED), child);
	UT_ASSERT(WIFCONTINUED(status));
	UT_ASSERT_EQ(kill(child, SIGKILL), 0);
	signal_expect_cld(CLD_KILLED, SIGKILL);
	UT_ASSERT(signal_cld_info.utime >= event_utime);
	UT_ASSERT(signal_cld_info.stime >= event_stime);
	UT_EXPECT_SIGNAL(child, SIGKILL);

	UT_ASSERT_EQ(sigaction(SIGCHLD, &old_action, NULL), 0);
}

UT_CASE(signal_sigchld_sigtimedwait_info, 5000)
{
	sigset_t blocked;
	sigset_t old_mask;
	siginfo_t info = {0};
	struct timespec timeout = {.tv_sec = 1};
	struct rusage usage = {0};
	pid_t child;
	int status;

	UT_ASSERT_EQ(sigemptyset(&blocked), 0);
	UT_ASSERT_EQ(sigaddset(&blocked, SIGCHLD), 0);
	UT_ASSERT_EQ(sigprocmask(SIG_BLOCK, &blocked, &old_mask), 0);
	child = UT_FORK();
	if (child == 0) {
		signal_burn_cpu(120);
		_exit(17);
	}
	UT_ASSERT_EQ(sigtimedwait(&blocked, &info, &timeout), SIGCHLD);
	UT_ASSERT_EQ(info.si_signo, SIGCHLD);
	UT_ASSERT_EQ(info.si_code, CLD_EXITED);
	UT_ASSERT_EQ(info.si_status, 17);
	UT_ASSERT_EQ(info.si_pid, child);
	UT_ASSERT(info.si_utime + info.si_stime > 0);
	UT_ASSERT_EQ(syscall(SYS_wait4, child, &status, 0, &usage), child);
	UT_ASSERT(WIFEXITED(status));
	UT_ASSERT_EQ(WEXITSTATUS(status), 17);
	UT_ASSERT_EQ(info.si_utime, signal_timeval_ticks(&usage.ru_utime));
	UT_ASSERT_EQ(info.si_stime, signal_timeval_ticks(&usage.ru_stime));
	UT_ASSERT_EQ(sigprocmask(SIG_SETMASK, &old_mask, NULL), 0);
}

UT_CASE(signal_sigchld_thread_group_cputime_snapshot, 5000)
{
	struct sigaction action = {
		.sa_sigaction = signal_cld_handler,
		.sa_flags = SA_SIGINFO,
	};
	struct sigaction old_action;
	int ready[2];
	char marker;
	pid_t child;
	int status;
	struct rusage usage = {0};
	clock_t stop_utime;
	clock_t stop_stime;
	clock_t final_utime;
	clock_t final_stime;

	UT_ASSERT_EQ(pipe(ready), 0);
	UT_ASSERT_EQ(sigemptyset(&action.sa_mask), 0);
	UT_ASSERT_EQ(sigaction(SIGCHLD, &action, &old_action), 0);
	signal_child_count = 0;
	child = UT_FORK();
	if (child == 0) {
		pthread_t worker;

		(void)close(ready[0]);
		if (pthread_create(&worker, NULL, signal_thread_group_worker,
				   &ready[1]) != 0)
			_exit(124);
		if (pthread_join(worker, NULL) != 0)
			_exit(123);
		if (write(ready[1], "J", 1) != 1)
			_exit(122);
		for (;;)
			signal_burn_cpu(1000);
	}
	(void)close(ready[1]);
	UT_ASSERT_EQ(read(ready[0], &marker, 1), 1);
	UT_ASSERT_EQ(marker, 'T');
	UT_ASSERT_EQ(read(ready[0], &marker, 1), 1);
	UT_ASSERT_EQ(marker, 'J');
	/* Let the still-live main task accrue time after the worker has
	 * exited and been folded into proc->cputime. */
	signal_sleep_ms(40);
	UT_ASSERT_EQ(kill(child, SIGSTOP), 0);
	signal_expect_cld_for_child(CLD_STOPPED, SIGSTOP, child);
	stop_utime = signal_cld_info.utime;
	stop_stime = signal_cld_info.stime;
	UT_ASSERT(stop_utime + stop_stime > 0);
	UT_ASSERT_EQ(waitpid(child, &status, WUNTRACED), child);
	UT_ASSERT(WIFSTOPPED(status));
	UT_ASSERT_EQ(kill(child, SIGCONT), 0);
	signal_expect_cld_for_child(CLD_CONTINUED, SIGCONT, child);
	UT_ASSERT(signal_cld_info.utime >= stop_utime);
	UT_ASSERT(signal_cld_info.stime >= stop_stime);
	UT_ASSERT_EQ(waitpid(child, &status, WCONTINUED), child);
	UT_ASSERT(WIFCONTINUED(status));
	UT_ASSERT_EQ(kill(child, SIGKILL), 0);
	signal_expect_cld_for_child(CLD_KILLED, SIGKILL, child);
	final_utime = signal_cld_info.utime;
	final_stime = signal_cld_info.stime;
	UT_ASSERT(final_utime >= stop_utime);
	UT_ASSERT(final_stime >= stop_stime);
	UT_ASSERT_EQ(syscall(SYS_wait4, child, &status, 0, &usage), child);
	UT_ASSERT(WIFSIGNALED(status));
	UT_ASSERT_EQ(WTERMSIG(status), SIGKILL);
	UT_ASSERT_EQ(final_utime, signal_timeval_ticks(&usage.ru_utime));
	UT_ASSERT_EQ(final_stime, signal_timeval_ticks(&usage.ru_stime));
	UT_ASSERT_EQ(close(ready[0]), 0);
	UT_ASSERT_EQ(sigaction(SIGCHLD, &old_action, NULL), 0);
}
