/* Cross-CPU scheduler wake regression. */

#include <errno.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <utest.h>

#define SMP_PIN_ATTEMPTS 100
#define SMP_PING_PONG_ROUNDS 1

static void smp_sleep_ms(long milliseconds)
{
	struct timespec delay = {
		.tv_sec = milliseconds / 1000,
		.tv_nsec = (milliseconds % 1000) * 1000000L,
	};

	while (nanosleep(&delay, &delay) < 0 && errno == EINTR)
		;
}

static void smp_pin_blocked(pid_t child, unsigned long mask)
{
	for (unsigned int attempt = 0; attempt < SMP_PIN_ATTEMPTS; attempt++) {
		errno = 0;
		if (syscall(SYS_sched_setaffinity, child, sizeof(mask), &mask) == 0)
			return;
		UT_ASSERT_EQ(errno, EBUSY);
		smp_sleep_ms(10);
	}
	UT_FAIL("child affinity remained busy");
}

static void smp_worker_zero(int ready_fd, int start_fd, int send_fd, int recv_fd)
{
	char byte;

	if (write(ready_fd, "0", 1) != 1)
		_exit(127);
	if (read(start_fd, &byte, 1) != 1)
		_exit(126);
	for (unsigned int round = 0; round < SMP_PING_PONG_ROUNDS; round++) {
		if (write(send_fd, "p", 1) != 1)
			_exit(125);
		if (read(recv_fd, &byte, 1) != 1)
			_exit(124);
	}
	_exit(0);
}

static void smp_worker_one(int ready_fd, int start_fd, int recv_fd, int send_fd)
{
	char byte;

	if (write(ready_fd, "1", 1) != 1)
		_exit(127);
	if (read(start_fd, &byte, 1) != 1)
		_exit(126);
	for (unsigned int round = 0; round < SMP_PING_PONG_ROUNDS; round++) {
		if (read(recv_fd, &byte, 1) != 1)
			_exit(125);
		if (write(send_fd, "q", 1) != 1)
			_exit(124);
	}
	_exit(0);
}

UT_CASE(smp_cross_cpu_wake, 5000)
{
	int ready[2];
	int start_zero[2];
	int start_one[2];
	int zero_to_one[2];
	int one_to_zero[2];
	pid_t zero;
	pid_t one;
	char byte;

	UT_ASSERT_EQ(pipe(ready), 0);
	UT_ASSERT_EQ(pipe(start_zero), 0);
	UT_ASSERT_EQ(pipe(start_one), 0);
	UT_ASSERT_EQ(pipe(zero_to_one), 0);
	UT_ASSERT_EQ(pipe(one_to_zero), 0);
	zero = UT_FORK();
	if (zero == 0) {
		close(ready[0]);
		close(start_zero[1]);
		close(start_one[0]);
		close(start_one[1]);
		close(zero_to_one[0]);
		close(one_to_zero[1]);
		smp_worker_zero(ready[1], start_zero[0], zero_to_one[1],
				one_to_zero[0]);
	}
	one = UT_FORK();
	if (one == 0) {
		close(ready[0]);
		close(start_zero[0]);
		close(start_zero[1]);
		close(start_one[1]);
		close(zero_to_one[1]);
		close(one_to_zero[0]);
		smp_worker_one(ready[1], start_one[0], zero_to_one[0],
			       one_to_zero[1]);
	}

	close(ready[1]);
	close(start_zero[0]);
	close(start_one[0]);
	close(zero_to_one[0]);
	close(zero_to_one[1]);
	close(one_to_zero[0]);
	close(one_to_zero[1]);
	UT_ASSERT_EQ(read(ready[0], &byte, 1), 1);
	UT_ASSERT_EQ(read(ready[0], &byte, 1), 1);
	close(ready[0]);

	smp_pin_blocked(zero, 1UL);
	smp_pin_blocked(one, 2UL);
	UT_ASSERT_EQ(write(start_zero[1], "s", 1), 1);
	UT_ASSERT_EQ(write(start_one[1], "s", 1), 1);
	close(start_zero[1]);
	close(start_one[1]);
	UT_EXPECT_EXIT(zero, 0);
	UT_EXPECT_EXIT(one, 0);
}
