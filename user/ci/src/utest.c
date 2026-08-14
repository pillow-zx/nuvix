#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <utest.h>

#include "utest_internal.h"

#define UT_EXEC_DIRECTORY "/usr/lib/nuvix-tests"

struct ut_context {
	const char *fixture;
	struct ut_case_result *result;
	const char *xfail_reason;
	const char *skip_reason;
	unsigned int failures;
	bool xfail;
	bool skipped;
	jmp_buf abort;
};

static struct ut_context *ut_current;

static void ut_store_reason(char destination[UT_REASON_SIZE], const char *reason)
{
	if (!reason)
		reason = "unspecified";
	(void)snprintf(destination, UT_REASON_SIZE, "%s", reason);
}

static void ut_finish_case(struct ut_context *context)
{
	struct ut_case_result *result = context->result;

	result->failures = context->failures;
	if (context->skipped) {
		result->status = UT_CASE_SKIP;
		ut_store_reason(result->reason, context->skip_reason);
		return;
	}
	if (context->xfail) {
		result->status = context->failures ? UT_CASE_XFAIL : UT_CASE_XPASS;
		ut_store_reason(result->reason, context->xfail_reason);
		return;
	}
	result->status = context->failures ? UT_CASE_FAIL : UT_CASE_PASS;
}

static void ut_abort_case(void) __attribute__((noreturn));

static void ut_abort_case(void)
{
	longjmp(ut_current->abort, 1);
}

bool ut_strings_equal(const char *actual, const char *expected)
{
	return actual && expected && strcmp(actual, expected) == 0;
}

void ut_record_failure_at(const char *file, int line, bool fatal,
			  const char *format, ...)
{
	va_list arguments;

	ut_current->failures++;
	fprintf(stderr, "[UTEST] %s:%d: ", file, line);
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
	if (fatal)
		ut_abort_case();
}

void ut_record_compare_at(const char *file, int line, bool fatal,
			  const char *actual_expr, const char *expected_expr,
			  uintmax_t actual, uintmax_t expected, bool equal)
{
	ut_record_failure_at(file, line, fatal,
			     "%s: %s=0x%jx, %s=0x%jx", equal ? "not equal" : "equal",
			     actual_expr, actual, expected_expr, expected);
}

void ut_record_string_at(const char *file, int line, bool fatal,
			 const char *actual_expr, const char *expected_expr,
			 const char *actual, const char *expected)
{
	ut_record_failure_at(file, line, fatal,
			     "strings differ: %s=\"%s\", %s=\"%s\"", actual_expr,
			     actual ? actual : "(null)", expected_expr,
			     expected ? expected : "(null)");
}

void ut_record_memory_at(const char *file, int line, bool fatal,
			 const char *actual_expr, const char *expected_expr,
			 const void *actual, const void *expected, size_t size)
{
	(void)actual;
	(void)expected;
	ut_record_failure_at(file, line, fatal,
			     "memory differs: %s and %s (%zu bytes)", actual_expr,
			     expected_expr, size);
}

void ut_record_errno_at(const char *file, int line, bool fatal,
			const char *expr, long result, int actual_errno,
			int expected_errno)
{
	ut_record_failure_at(file, line, fatal,
			     "errno mismatch: %s returned %ld, errno=%d, expected -1/%d",
			     expr, result, actual_errno, expected_errno);
}

void ut_skip_at(const char *file, int line, const char *reason)
{
	if (ut_current->failures || ut_current->xfail)
		ut_record_failure_at(file, line, true,
				     "UT_SKIP cannot hide a failure or expected failure");
	ut_current->skipped = true;
	ut_current->skip_reason = reason;
	ut_abort_case();
}

void ut_xfail_at(const char *file, int line, const char *reason)
{
	if (ut_current->xfail || ut_current->skipped || ut_current->failures)
		ut_record_failure_at(file, line, true,
				     "UT_XFAIL must precede all case failures");
	ut_current->xfail = true;
	ut_current->xfail_reason = reason;
}

pid_t ut_fork_at(const char *file, int line)
{
	pid_t child = fork();

	if (child < 0)
		ut_record_failure_at(file, line, true, "fork failed: errno=%d", errno);
	return child;
}

int ut_wait_child_at(const char *file, int line, pid_t child)
{
	int status;

	while (waitpid(child, &status, 0) < 0) {
		if (errno == EINTR)
			continue;
		ut_record_failure_at(file, line, true,
				     "waitpid(%ld) failed: errno=%d", (long)child, errno);
	}
	return status;
}

void ut_expect_exit_at(const char *file, int line, pid_t child, int code)
{
	int status = ut_wait_child_at(file, line, child);

	if (!WIFEXITED(status) || WEXITSTATUS(status) != code)
		ut_record_failure_at(file, line, false,
				     "child %ld exit status %#x, expected exit %d",
				     (long)child, status, code);
}

void ut_expect_signal_at(const char *file, int line, pid_t child, int signal)
{
	int status = ut_wait_child_at(file, line, child);

	if (!WIFSIGNALED(status) || WTERMSIG(status) != signal)
		ut_record_failure_at(file, line, false,
				     "child %ld exit status %#x, expected signal %d",
				     (long)child, status, signal);
}

static bool ut_valid_relative_path(const char *path)
{
	const char *component;
	const char *slash;

	if (!path || !*path || path[0] == '/')
		return false;
	component = path;
	while (*component) {
		slash = strchr(component, '/');
		if (!slash)
			slash = component + strlen(component);
		if (slash == component ||
		    ((size_t)(slash - component) == 1 && component[0] == '.') ||
		    ((size_t)(slash - component) == 2 && component[0] == '.' &&
		     component[1] == '.'))
			return false;
		if (!*slash)
			break;
		component = slash + 1;
	}
	return true;
}

static char *ut_join_path(const char *directory, const char *relative_path)
{
	size_t directory_size;
	size_t relative_size;
	char *path;

	if (!ut_valid_relative_path(relative_path)) {
		errno = EINVAL;
		return NULL;
	}
	directory_size = strlen(directory);
	relative_size = strlen(relative_path);
	if (directory_size > SIZE_MAX - relative_size - 2) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	path = malloc(directory_size + relative_size + 2);
	if (!path)
		return NULL;
	(void)snprintf(path, directory_size + relative_size + 2, "%s/%s",
		       directory, relative_path);
	return path;
}

const char *ut_fixture_root(void)
{
	return ut_current ? ut_current->fixture : NULL;
}

char *ut_path(const char *relative_path)
{
	if (!ut_current) {
		errno = EINVAL;
		return NULL;
	}
	return ut_join_path(ut_current->fixture, relative_path);
}

char *ut_exec_path(const char *name)
{
	return ut_join_path(UT_EXEC_DIRECTORY, name);
}

int ut_mkdir(const char *relative_path, mode_t mode)
{
	char *path = ut_path(relative_path);
	int result;

	if (!path)
		return -1;
	result = mkdir(path, mode);
	free(path);
	return result;
}

int ut_write_file(const char *relative_path, const void *data, size_t size,
		  mode_t mode)
{
	char *path = ut_path(relative_path);
	const unsigned char *buffer = data;
	int fd;
	int result = -1;
	size_t written = 0;

	if (!path)
		return -1;
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	free(path);
	if (fd < 0)
		return -1;
	while (written < size) {
		ssize_t count = write(fd, buffer + written, size - written);

		if (count < 0) {
			if (errno == EINTR)
				continue;
			goto out;
		}
		if (count == 0) {
			errno = EIO;
			goto out;
		}
		written += (size_t)count;
	}
	result = 0;
out:
	if (close(fd) < 0 && result == 0)
		result = -1;
	return result;
}

char *ut_read_file(const char *relative_path, size_t *size_out)
{
	char *path = ut_path(relative_path);
	struct stat st;
	char *buffer;
	int fd;
	size_t read_size = 0;

	if (!path)
		return NULL;
	fd = open(path, O_RDONLY);
	free(path);
	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) < 0 || st.st_size < 0 ||
	    (uintmax_t)st.st_size > SIZE_MAX - 1)
		goto fail;
	buffer = malloc((size_t)st.st_size + 1);
	if (!buffer)
		goto fail;
	while (read_size < (size_t)st.st_size) {
		ssize_t count = read(fd, buffer + read_size,
				     (size_t)st.st_size - read_size);

		if (count < 0) {
			if (errno == EINTR)
				continue;
			free(buffer);
			goto fail;
		}
		if (count == 0)
			break;
		read_size += (size_t)count;
	}
	buffer[read_size] = '\0';
	if (close(fd) < 0) {
		free(buffer);
		return NULL;
	}
	if (size_out)
		*size_out = read_size;
	return buffer;
fail:
	(void)close(fd);
	return NULL;
}

static int ut_remove_tree_path(const char *path)
{
	struct stat st;

	if (lstat(path, &st) < 0)
		return -1;
	if (!S_ISDIR(st.st_mode))
		return unlink(path);
	DIR *directory = opendir(path);
	struct dirent *entry;
	int result = 0;

	if (!directory)
		return -1;
	while ((entry = readdir(directory)) != NULL) {
		char *child;
		size_t path_size;
		size_t name_size;

		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;
		path_size = strlen(path);
		name_size = strlen(entry->d_name);
		child = malloc(path_size + name_size + 2);
		if (!child) {
			result = -1;
			break;
		}
		(void)snprintf(child, path_size + name_size + 2, "%s/%s", path,
			       entry->d_name);
		if (ut_remove_tree_path(child) < 0)
			result = -1;
		free(child);
		if (result < 0)
			break;
	}
	if (closedir(directory) < 0 && result == 0)
		result = -1;
	if (result < 0)
		return -1;
	return rmdir(path);
}

int ut_remove_tree(const char *relative_path)
{
	char *path = ut_path(relative_path);
	int result;

	if (!path)
		return -1;
	result = ut_remove_tree_path(path);
	free(path);
	return result;
}

int ut_cleanup_fixture(const char *fixture)
{
	return ut_remove_tree_path(fixture);
}

void ut_run_case(const struct ut_case *test_case, const char *fixture,
		 struct ut_case_result *result)
{
	struct ut_context context = {
		.fixture = fixture,
		.result = result,
	};

	memset(result, 0, sizeof(*result));
	ut_current = &context;
	if (setjmp(context.abort) == 0)
		test_case->fn();
	ut_finish_case(&context);
	fflush(NULL);
}
