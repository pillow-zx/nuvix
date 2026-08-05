#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <utest.h>

static volatile sig_atomic_t time_signal_count;

struct time_accounting_reader {
	atomic_bool stop;
	atomic_int failed;
};

static void time_signal_handler(int signal)
{
	(void)signal;
	time_signal_count++;
}

static bool time_timespec_not_before(const struct timespec *left,
				     const struct timespec *right)
{
	return left->tv_sec > right->tv_sec ||
	       (left->tv_sec == right->tv_sec &&
		left->tv_nsec >= right->tv_nsec);
}

static bool time_timeval_not_before(const struct timeval *left,
				    const struct timeval *right)
{
	return left->tv_sec > right->tv_sec ||
	       (left->tv_sec == right->tv_sec &&
		left->tv_usec >= right->tv_usec);
}

static long time_timeval_ticks(const struct timeval *timeval)
{
	return timeval->tv_sec * 100L + timeval->tv_usec / 10000L;
}

static void time_burn_cpu(unsigned int milliseconds)
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

static void *time_read_child_accounting(void *argument)
{
	struct time_accounting_reader *reader = argument;
	struct tms previous_times = {};
	struct rusage previous_usage = {};

	while (!atomic_load_explicit(&reader->stop, memory_order_acquire)) {
		struct tms current_times;
		struct rusage current_usage;

		if (times(&current_times) == (clock_t)-1 ||
		    getrusage(RUSAGE_CHILDREN, &current_usage) < 0 ||
		    current_times.tms_cutime < previous_times.tms_cutime ||
		    current_times.tms_cstime < previous_times.tms_cstime ||
		    !time_timeval_not_before(&current_usage.ru_utime,
					     &previous_usage.ru_utime) ||
		    !time_timeval_not_before(&current_usage.ru_stime,
					     &previous_usage.ru_stime)) {
			atomic_store_explicit(&reader->failed, 1,
					      memory_order_release);
			break;
		}
		previous_times = current_times;
		previous_usage = current_usage;
	}
	return NULL;
}

UT_CASE(time_clocks_and_realtime_write, 5000)
{
	struct timespec monotonic_before;
	struct timespec monotonic_after;
	struct timespec realtime;
	struct timespec resolution;
	struct timeval timeval;
	struct stat st;
	struct timespec target = {
		.tv_sec = 1700000000,
		.tv_nsec = 123456789,
	};
	char *path;
	pid_t child;

	UT_ASSERT_EQ(clock_gettime(CLOCK_MONOTONIC, &monotonic_before), 0);
	UT_ASSERT_EQ(nanosleep(&(struct timespec){.tv_nsec = 20000000}, NULL),
		     0);
	UT_ASSERT_EQ(clock_gettime(CLOCK_MONOTONIC, &monotonic_after), 0);
	UT_EXPECT(
		time_timespec_not_before(&monotonic_after, &monotonic_before));
	UT_ASSERT_EQ(clock_getres(CLOCK_MONOTONIC, &resolution), 0);
	UT_EXPECT(resolution.tv_sec > 0 || resolution.tv_nsec > 0);
	UT_ASSERT_EQ(clock_settime(CLOCK_REALTIME, &target), 0);
	UT_ASSERT_EQ(clock_gettime(CLOCK_REALTIME, &realtime), 0);
	UT_EXPECT(time_timespec_not_before(&realtime, &target));
	UT_EXPECT(realtime.tv_sec <= target.tv_sec + 1);
	UT_EXPECT(monotonic_after.tv_sec < target.tv_sec);
	UT_ASSERT_EQ(syscall(SYS_gettimeofday, &timeval, NULL), 0);
	UT_EXPECT(timeval.tv_sec >= target.tv_sec);
	UT_EXPECT(timeval.tv_sec <= target.tv_sec + 1);

	UT_ASSERT_EQ(ut_write_file("realtime-timestamp", "x", 1, 0600), 0);
	path = ut_path("realtime-timestamp");
	UT_ASSERT(path != NULL);
	UT_ASSERT_EQ(stat(path, &st), 0);
	free(path);
	UT_EXPECT(st.st_mtime >= target.tv_sec);
	UT_EXPECT(st.st_ctime >= target.tv_sec);

	UT_EXPECT_ERRNO(
		clock_settime(CLOCK_REALTIME,
			      &(struct timespec){.tv_nsec = 1000000000}),
		EINVAL);
	UT_EXPECT_ERRNO(
		clock_settime(CLOCK_REALTIME, &(struct timespec){.tv_sec = -1}),
		EINVAL);
	UT_EXPECT_ERRNO(clock_settime(CLOCK_REALTIME, &(struct timespec){0}),
			EINVAL);
	UT_EXPECT_ERRNO(clock_settime(CLOCK_MONOTONIC, &target), EINVAL);
	UT_EXPECT_EQ(
		clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &target, NULL),
		EINVAL);

	child = UT_FORK();
	if (child == 0) {
		if (setuid(1000) < 0)
			_exit(125);
		errno = 0;
		if (clock_settime(CLOCK_REALTIME, &target) != -1 ||
		    errno != EPERM)
			_exit(1);
		_exit(0);
	}
	UT_EXPECT_EXIT(child, 0);
}

UT_CASE(time_nanosleep_interrupt_remainder, 5000)
{
	struct sigaction action = {
		.sa_handler = time_signal_handler,
	};
	struct sigaction old_action;
	struct timespec requested = {
		.tv_nsec = 200000000,
	};
	struct timespec remainder = {};
	pid_t child;

	UT_ASSERT_EQ(sigemptyset(&action.sa_mask), 0);
	UT_ASSERT_EQ(sigaction(SIGUSR1, &action, &old_action), 0);
	time_signal_count = 0;
	child = UT_FORK();
	if (child == 0) {
		(void)nanosleep(&(struct timespec){.tv_nsec = 30000000}, NULL);
		(void)kill(getppid(), SIGUSR1);
		_exit(0);
	}
	UT_EXPECT_ERRNO(nanosleep(&requested, &remainder), EINTR);
	UT_EXPECT(time_signal_count > 0);
	UT_EXPECT(remainder.tv_sec > 0 || remainder.tv_nsec > 0);
	UT_EXPECT(remainder.tv_sec < requested.tv_sec ||
		  (remainder.tv_sec == requested.tv_sec &&
		   remainder.tv_nsec <= requested.tv_nsec));
	UT_EXPECT_EXIT(child, 0);
	UT_ASSERT_EQ(sigaction(SIGUSR1, &old_action, NULL), 0);
}

UT_CASE(time_child_cputime_accounting, 5000)
{
	struct time_accounting_reader reader = {
		.stop = false,
		.failed = 0,
	};
	struct tms before_times;
	struct tms after_times;
	struct rusage before_usage;
	struct rusage after_usage;
	pthread_t reader_thread;
	pid_t child;

	UT_ASSERT(times(&before_times) != (clock_t)-1);
	UT_ASSERT_EQ(getrusage(RUSAGE_CHILDREN, &before_usage), 0);
	child = UT_FORK();
	if (child == 0) {
		time_burn_cpu(250);
		_exit(0);
	}
	UT_ASSERT_EQ(pthread_create(&reader_thread, NULL,
				    time_read_child_accounting, &reader),
		     0);
	UT_EXPECT_EXIT(child, 0);
	atomic_store_explicit(&reader.stop, true, memory_order_release);
	UT_ASSERT_EQ(pthread_join(reader_thread, NULL), 0);
	UT_EXPECT_EQ(atomic_load_explicit(&reader.failed, memory_order_acquire),
		     0);

	UT_ASSERT(times(&after_times) != (clock_t)-1);
	UT_ASSERT_EQ(getrusage(RUSAGE_CHILDREN, &after_usage), 0);
	UT_EXPECT(after_times.tms_cutime + after_times.tms_cstime >
		  before_times.tms_cutime + before_times.tms_cstime);
	UT_EXPECT_EQ(after_times.tms_cutime,
		     time_timeval_ticks(&after_usage.ru_utime));
	UT_EXPECT_EQ(after_times.tms_cstime,
		     time_timeval_ticks(&after_usage.ru_stime));
}

/*
 * setitimer / POSIX timer_create/settime/delete are postponed by DESIGN.md
 * and return -ENOSYS; sigtimedwait itself is covered by signal_* cases.
 */
