#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <utest.h>

#include "utest_internal.h"

#define UT_FIXTURE_DIRECTORY "/tmp/nuvix-tests"
#define UT_POLL_INTERVAL_MS 5

extern const struct ut_case __start_ut_cases[];
extern const struct ut_case __stop_ut_cases[];

enum ut_runner_status {
	UT_RUNNER_PASS,
	UT_RUNNER_FAIL,
	UT_RUNNER_SKIP,
	UT_RUNNER_XFAIL,
	UT_RUNNER_XPASS,
	UT_RUNNER_CRASH,
	UT_RUNNER_TIMEOUT,
};

struct ut_summary {
	unsigned int pass;
	unsigned int fail;
	unsigned int skip;
	unsigned int xfail;
	unsigned int xpass;
	unsigned int crash;
	unsigned int timeout;
};

static const char *ut_status_name(enum ut_runner_status status)
{
	switch (status) {
	case UT_RUNNER_PASS:
		return "PASS";
	case UT_RUNNER_FAIL:
		return "FAIL";
	case UT_RUNNER_SKIP:
		return "SKIP";
	case UT_RUNNER_XFAIL:
		return "XFAIL";
	case UT_RUNNER_XPASS:
		return "XPASS";
	case UT_RUNNER_CRASH:
		return "CRASH";
	case UT_RUNNER_TIMEOUT:
		return "TIMEOUT";
	}
	return "UNKNOWN";
}

static void ut_summary_add(struct ut_summary *summary,
			   enum ut_runner_status status)
{
	switch (status) {
	case UT_RUNNER_PASS:
		summary->pass++;
		break;
	case UT_RUNNER_FAIL:
		summary->fail++;
		break;
	case UT_RUNNER_SKIP:
		summary->skip++;
		break;
	case UT_RUNNER_XFAIL:
		summary->xfail++;
		break;
	case UT_RUNNER_XPASS:
		summary->xpass++;
		break;
	case UT_RUNNER_CRASH:
		summary->crash++;
		break;
	case UT_RUNNER_TIMEOUT:
		summary->timeout++;
		break;
	}
}

static unsigned long long ut_elapsed_ms(const struct timespec *start,
					const struct timespec *end)
{
	int64_t seconds = (int64_t)end->tv_sec - (int64_t)start->tv_sec;
	int64_t nanoseconds = (int64_t)end->tv_nsec - (int64_t)start->tv_nsec;

	if (nanoseconds < 0) {
		seconds--;
		nanoseconds += 1000000000LL;
	}
	if (seconds < 0)
		return 0;
	return (unsigned long long)seconds * 1000ULL +
	       (unsigned long long)nanoseconds / 1000000ULL;
}

static void ut_case_name_component(char *destination, size_t destination_size,
				   const char *name)
{
	size_t index;

	if (destination_size == 0)
		return;
	for (index = 0; name[index] && index + 1 < destination_size; index++) {
		char character = name[index];

		if ((character >= 'a' && character <= 'z') ||
		    (character >= 'A' && character <= 'Z') ||
		    (character >= '0' && character <= '9') || character == '_')
			destination[index] = character;
		else
			destination[index] = '_';
	}
	destination[index] = '\0';
}

static char *ut_make_fixture(unsigned int ordinal, const char *name)
{
	char component[96];
	int length;
	char *path;

	ut_case_name_component(component, sizeof(component), name);
	length = snprintf(NULL, 0, "%s/%03u-%s", UT_FIXTURE_DIRECTORY,
			  ordinal, component);
	if (length < 0) {
		errno = EINVAL;
		return NULL;
	}
	path = malloc((size_t)length + 1);
	if (!path)
		return NULL;
	(void)snprintf(path, (size_t)length + 1, "%s/%03u-%s",
		       UT_FIXTURE_DIRECTORY, ordinal, component);
	if (mkdir(path, 0700) < 0) {
		free(path);
		return NULL;
	}
	return path;
}

static int ut_write_result(int fd, const struct ut_case_result *result)
{
	const unsigned char *buffer = (const unsigned char *)result;
	size_t offset = 0;

	while (offset < sizeof(*result)) {
		ssize_t written = write(fd, buffer + offset, sizeof(*result) - offset);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (written == 0) {
			errno = EIO;
			return -1;
		}
		offset += (size_t)written;
	}
	return 0;
}

static bool ut_read_result(int fd, struct ut_case_result *result)
{
	ssize_t count;

	do {
		count = read(fd, result, sizeof(*result));
	} while (count < 0 && errno == EINTR);
	return count == (ssize_t)sizeof(*result);
}

static bool ut_isolated_case_group(pid_t child)
{
	if (setpgid(child, child) == 0)
		return true;
	return getpgid(child) == child;
}

static void ut_kill_case_group(pid_t child, bool isolated_group)
{
	if (isolated_group)
		(void)kill(-child, SIGKILL);
	else
		(void)kill(child, SIGKILL);
}

static enum ut_runner_status
ut_status_from_result(const struct ut_case_result *result)
{
	switch (result->status) {
	case UT_CASE_PASS:
		return UT_RUNNER_PASS;
	case UT_CASE_FAIL:
		return UT_RUNNER_FAIL;
	case UT_CASE_SKIP:
		return UT_RUNNER_SKIP;
	case UT_CASE_XFAIL:
		return UT_RUNNER_XFAIL;
	case UT_CASE_XPASS:
		return UT_RUNNER_XPASS;
	}
	return UT_RUNNER_CRASH;
}

static enum ut_runner_status ut_run_one(const struct ut_case *test_case,
					unsigned int ordinal,
					unsigned long long *duration_ms,
					char reason[UT_REASON_SIZE])
{
	struct timespec start;
	struct timespec now;
	struct timespec pause = {
		.tv_nsec = UT_POLL_INTERVAL_MS * 1000000L,
	};
	struct ut_case_result result;
	char *fixture;
	int result_pipe[2];
	int sync_pipe[2];
	pid_t child;
	int wait_status = 0;
	bool timed_out = false;
	bool isolated_group;
	bool got_result;

	reason[0] = '\0';
	fixture = ut_make_fixture(ordinal, test_case->name);
	if (!fixture) {
		(void)snprintf(reason, UT_REASON_SIZE, "fixture setup: errno=%d", errno);
		return UT_RUNNER_FAIL;
	}
	if (pipe2(result_pipe, O_CLOEXEC | O_NONBLOCK) < 0) {
		(void)snprintf(reason, UT_REASON_SIZE, "result pipe: errno=%d", errno);
		(void)ut_cleanup_fixture(fixture);
		free(fixture);
		return UT_RUNNER_FAIL;
	}
	if (pipe2(sync_pipe, O_CLOEXEC) < 0) {
		(void)snprintf(reason, UT_REASON_SIZE, "sync pipe: errno=%d", errno);
		(void)close(result_pipe[0]);
		(void)close(result_pipe[1]);
		(void)ut_cleanup_fixture(fixture);
		free(fixture);
		return UT_RUNNER_FAIL;
	}
	if (clock_gettime(CLOCK_MONOTONIC, &start) < 0) {
		(void)snprintf(reason, UT_REASON_SIZE, "clock_gettime: errno=%d", errno);
		(void)close(result_pipe[0]);
		(void)close(result_pipe[1]);
		(void)close(sync_pipe[0]);
		(void)close(sync_pipe[1]);
		(void)ut_cleanup_fixture(fixture);
		free(fixture);
		return UT_RUNNER_FAIL;
	}
	child = fork();
	if (child < 0) {
		(void)snprintf(reason, UT_REASON_SIZE, "fork: errno=%d", errno);
		(void)close(result_pipe[0]);
		(void)close(result_pipe[1]);
		(void)close(sync_pipe[0]);
		(void)close(sync_pipe[1]);
		(void)ut_cleanup_fixture(fixture);
		free(fixture);
		return UT_RUNNER_FAIL;
	}
	if (child == 0) {
		(void)close(result_pipe[0]);
		(void)close(sync_pipe[1]);
		if (setpgid(0, 0) < 0)
			_exit(126);
		/* Hold the child until the runner has checked its process group:
		 * the isolation check must never race a fast-exiting case. */
		{
			char sync_byte;
			ssize_t sync_count;

			do {
				sync_count = read(sync_pipe[0], &sync_byte, 1);
			} while (sync_count < 0 && errno == EINTR);
			(void)sync_count;
		}
		(void)close(sync_pipe[0]);
		if (chdir(fixture) < 0)
			_exit(126);
		ut_run_case(test_case, fixture, &result);
		if (ut_write_result(result_pipe[1], &result) < 0)
			_exit(127);
		(void)close(result_pipe[1]);
		_exit(0);
	}

	(void)close(result_pipe[1]);
	(void)close(sync_pipe[0]);
	isolated_group = ut_isolated_case_group(child);
	/* Release the child either way: on the failure path it is still
	 * blocked in the sync read and SIGKILL will take it. */
	(void)close(sync_pipe[1]);
	if (!isolated_group) {
		ut_kill_case_group(child, false);
		while (waitpid(child, &wait_status, 0) < 0 && errno == EINTR)
			;
		(void)close(result_pipe[0]);
		(void)ut_cleanup_fixture(fixture);
		free(fixture);
		(void)snprintf(reason, UT_REASON_SIZE,
			       "could not isolate the case process group: errno=%d", errno);
		return UT_RUNNER_CRASH;
	}
	for (;;) {
		pid_t waited = waitpid(child, &wait_status, WNOHANG);

		if (waited == child)
			break;
		if (waited < 0 && errno != EINTR) {
			ut_kill_case_group(child, true);
			(void)snprintf(reason, UT_REASON_SIZE, "waitpid: errno=%d", errno);
			break;
		}
		if (clock_gettime(CLOCK_MONOTONIC, &now) < 0 ||
		    ut_elapsed_ms(&start, &now) >= test_case->timeout_ms) {
			timed_out = true;
			ut_kill_case_group(child, true);
			while (waitpid(child, &wait_status, 0) < 0 && errno == EINTR)
				;
			break;
		}
		(void)nanosleep(&pause, NULL);
	}
	if (!timed_out)
		ut_kill_case_group(child, true);
	if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
		*duration_ms = ut_elapsed_ms(&start, &now);
	else
		*duration_ms = 0;
	got_result = ut_read_result(result_pipe[0], &result);
	(void)close(result_pipe[0]);
	if (ut_cleanup_fixture(fixture) < 0) {
		free(fixture);
		(void)snprintf(reason, UT_REASON_SIZE, "fixture cleanup: errno=%d", errno);
		return UT_RUNNER_FAIL;
	}
	free(fixture);
	if (timed_out) {
		(void)snprintf(reason, UT_REASON_SIZE, "deadline %ums", test_case->timeout_ms);
		return UT_RUNNER_TIMEOUT;
	}
	if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0 ||
	    !got_result) {
		(void)snprintf(reason, UT_REASON_SIZE, "child status %#x", wait_status);
		return UT_RUNNER_CRASH;
	}
	if (result.reason[0])
		(void)snprintf(reason, UT_REASON_SIZE, "%s", result.reason);
	return ut_status_from_result(&result);
}

static bool ut_case_selected(const struct ut_case *test_case, const char *filter)
{
	return !filter || strcmp(test_case->name, filter) == 0;
}

static void ut_print_usage(const char *program)
{
	fprintf(stderr, "usage: %s [--list] [--case NAME]\n", program);
}

int main(int argc, char **argv)
{
	const char *filter = NULL;
	bool list_only = false;
	struct ut_summary summary = {};
	const struct ut_case *test_case;
	unsigned int ordinal = 0;
	int argument;

	setvbuf(stdout, NULL, _IOLBF, 0);
	for (argument = 1; argument < argc; argument++) {
		if (strcmp(argv[argument], "--list") == 0) {
			list_only = true;
			continue;
		}
		if (strcmp(argv[argument], "--case") == 0 && argument + 1 < argc) {
			filter = argv[++argument];
			continue;
		}
		ut_print_usage(argv[0]);
		return 2;
	}
	if (mkdir(UT_FIXTURE_DIRECTORY, 0700) < 0 && errno != EEXIST) {
		fprintf(stderr, "[UTEST] cannot create fixture root: errno=%d\n", errno);
		return 1;
	}
	for (test_case = __start_ut_cases; test_case < __stop_ut_cases;
	     test_case++) {
		char reason[UT_REASON_SIZE];
		unsigned long long duration_ms;
		enum ut_runner_status status;

		if (!ut_case_selected(test_case, filter))
			continue;
		if (list_only) {
			printf("%s %u\n", test_case->name, test_case->timeout_ms);
			continue;
		}
		ordinal++;
		status = ut_run_one(test_case, ordinal, &duration_ms, reason);
		ut_summary_add(&summary, status);
		printf("[UTEST] %s %s %llums", ut_status_name(status),
		       test_case->name, duration_ms);
		if (reason[0])
			printf(" %s", reason);
		putchar('\n');
	}
	if (list_only)
		return 0;
	if (filter && ordinal == 0) {
		fprintf(stderr, "[UTEST] unknown case: %s\n", filter);
		summary.fail++;
	}
	printf("[UTEST] done pass=%u fail=%u skip=%u xfail=%u xpass=%u crash=%u "
	       "timeout=%u\n",
	       summary.pass, summary.fail, summary.skip, summary.xfail,
	       summary.xpass, summary.crash, summary.timeout);
	fflush(NULL);
	sync();
	execl("/sbin/poweroff", "poweroff", (char *)NULL);
	fprintf(stderr, "[UTEST] poweroff failed: errno=%d\n", errno);
	return 1;
}

