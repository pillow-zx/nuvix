#ifndef _NUVIX_UTEST_H
#define _NUVIX_UTEST_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

struct ut_case {
	const char *name;
	unsigned int timeout_ms;
	void (*fn)(void);
};

bool ut_strings_equal(const char *actual, const char *expected);
void ut_record_failure_at(const char *file, int line, bool fatal,
			  const char *format, ...)
	__attribute__((format(printf, 4, 5)));
void ut_record_compare_at(const char *file, int line, bool fatal,
			  const char *actual_expr, const char *expected_expr,
			  uintmax_t actual, uintmax_t expected, bool equal);
void ut_record_string_at(const char *file, int line, bool fatal,
			 const char *actual_expr, const char *expected_expr,
			 const char *actual, const char *expected);
void ut_record_memory_at(const char *file, int line, bool fatal,
			 const char *actual_expr, const char *expected_expr,
			 const void *actual, const void *expected, size_t size);
void ut_record_errno_at(const char *file, int line, bool fatal,
			const char *expr, long result, int actual_errno,
			int expected_errno);
void ut_skip_at(const char *file, int line, const char *reason)
	__attribute__((noreturn));
void ut_xfail_at(const char *file, int line, const char *reason);

pid_t ut_fork_at(const char *file, int line);
int ut_wait_child_at(const char *file, int line, pid_t child);
void ut_expect_exit_at(const char *file, int line, pid_t child, int code);
void ut_expect_signal_at(const char *file, int line, pid_t child, int signal);

const char *ut_fixture_root(void);
char *ut_path(const char *relative_path);
char *ut_exec_path(const char *name);
int ut_mkdir(const char *relative_path, mode_t mode);
int ut_write_file(const char *relative_path, const void *data, size_t size,
		  mode_t mode);
char *ut_read_file(const char *relative_path, size_t *size_out);
int ut_remove_tree(const char *relative_path);

#define UT_CASE(case_name, timeout_value)                                      \
	static void case_name(void);                                           \
	static const struct ut_case ut_case_##case_name                        \
		__attribute__((used, section("ut_cases"))) = {                 \
			.name = #case_name,                                    \
			.timeout_ms = (timeout_value),                         \
			.fn = case_name,                                       \
	};                                                                     \
	static void case_name(void)

#define UT_ASSERT(condition)                                                   \
	do {                                                                   \
		if (!(condition))                                              \
			ut_record_failure_at(__FILE__, __LINE__, true,         \
					     "assertion failed: %s",           \
					     #condition);                      \
	} while (0)

#define UT_EXPECT(condition)                                                   \
	do {                                                                   \
		if (!(condition))                                              \
			ut_record_failure_at(__FILE__, __LINE__, false,        \
					     "expectation failed: %s",         \
					     #condition);                      \
	} while (0)

#define UT_ASSERT_EQ(actual, expected)                                         \
	do {                                                                   \
		__auto_type ut_actual_ = (actual);                             \
		__auto_type ut_expected_ = (expected);                         \
		if (!(ut_actual_ == ut_expected_))                             \
			ut_record_compare_at(__FILE__, __LINE__, true,         \
					     #actual, #expected,               \
					     (uintmax_t)ut_actual_,            \
					     (uintmax_t)ut_expected_, true);   \
	} while (0)

#define UT_EXPECT_EQ(actual, expected)                                         \
	do {                                                                   \
		__auto_type ut_actual_ = (actual);                             \
		__auto_type ut_expected_ = (expected);                         \
		if (!(ut_actual_ == ut_expected_))                             \
			ut_record_compare_at(__FILE__, __LINE__, false,        \
					     #actual, #expected,               \
					     (uintmax_t)ut_actual_,            \
					     (uintmax_t)ut_expected_, true);   \
	} while (0)

#define UT_ASSERT_NE(actual, expected)                                         \
	do {                                                                   \
		__auto_type ut_actual_ = (actual);                             \
		__auto_type ut_expected_ = (expected);                         \
		if (ut_actual_ == ut_expected_)                                \
			ut_record_compare_at(__FILE__, __LINE__, true,         \
					     #actual, #expected,               \
					     (uintmax_t)ut_actual_,            \
					     (uintmax_t)ut_expected_, false);  \
	} while (0)

#define UT_EXPECT_NE(actual, expected)                                         \
	do {                                                                   \
		__auto_type ut_actual_ = (actual);                             \
		__auto_type ut_expected_ = (expected);                         \
		if (ut_actual_ == ut_expected_)                                \
			ut_record_compare_at(__FILE__, __LINE__, false,        \
					     #actual, #expected,               \
					     (uintmax_t)ut_actual_,            \
					     (uintmax_t)ut_expected_, false);  \
	} while (0)

#define UT_ASSERT_STREQ(actual, expected)                                      \
	do {                                                                   \
		const char *ut_actual_ = (actual);                             \
		const char *ut_expected_ = (expected);                         \
		if (!ut_strings_equal(ut_actual_, ut_expected_))               \
			ut_record_string_at(__FILE__, __LINE__, true, #actual, \
					    #expected, ut_actual_,             \
					    ut_expected_);                     \
	} while (0)

#define UT_EXPECT_STREQ(actual, expected)                                      \
	do {                                                                   \
		const char *ut_actual_ = (actual);                             \
		const char *ut_expected_ = (expected);                         \
		if (!ut_strings_equal(ut_actual_, ut_expected_))               \
			ut_record_string_at(__FILE__, __LINE__, false,         \
					    #actual, #expected, ut_actual_,    \
					    ut_expected_);                     \
	} while (0)

#define UT_ASSERT_MEMEQ(actual, expected, size)                                \
	do {                                                                   \
		const void *ut_actual_ = (actual);                             \
		const void *ut_expected_ = (expected);                         \
		size_t ut_size_ = (size);                                      \
		if (memcmp(ut_actual_, ut_expected_, ut_size_) != 0)           \
			ut_record_memory_at(__FILE__, __LINE__, true, #actual, \
					    #expected, ut_actual_,             \
					    ut_expected_, ut_size_);           \
	} while (0)

#define UT_EXPECT_MEMEQ(actual, expected, size)                                \
	do {                                                                   \
		const void *ut_actual_ = (actual);                             \
		const void *ut_expected_ = (expected);                         \
		size_t ut_size_ = (size);                                      \
		if (memcmp(ut_actual_, ut_expected_, ut_size_) != 0)           \
			ut_record_memory_at(__FILE__, __LINE__, false,         \
					    #actual, #expected, ut_actual_,    \
					    ut_expected_, ut_size_);           \
	} while (0)

#define UT_ASSERT_ERRNO(expr, expected_errno)                                  \
	do {                                                                   \
		errno = 0;                                                     \
		long ut_result_ = (long)(expr);                                \
		int ut_errno_ = errno;                                         \
		int ut_expected_errno_ = (expected_errno);                     \
		if (ut_result_ != -1 || ut_errno_ != ut_expected_errno_)       \
			ut_record_errno_at(__FILE__, __LINE__, true, #expr,    \
					   ut_result_, ut_errno_,              \
					   ut_expected_errno_);                \
	} while (0)

#define UT_EXPECT_ERRNO(expr, expected_errno)                                  \
	do {                                                                   \
		errno = 0;                                                     \
		long ut_result_ = (long)(expr);                                \
		int ut_errno_ = errno;                                         \
		int ut_expected_errno_ = (expected_errno);                     \
		if (ut_result_ != -1 || ut_errno_ != ut_expected_errno_)       \
			ut_record_errno_at(__FILE__, __LINE__, false, #expr,   \
					   ut_result_, ut_errno_,              \
					   ut_expected_errno_);                \
	} while (0)

#define UT_FAIL(...) ut_record_failure_at(__FILE__, __LINE__, true, __VA_ARGS__)
#define UT_SKIP(reason)	 ut_skip_at(__FILE__, __LINE__, (reason))
#define UT_XFAIL(reason) ut_xfail_at(__FILE__, __LINE__, (reason))

#define UT_FORK()	     ut_fork_at(__FILE__, __LINE__)
#define UT_WAIT_CHILD(child) ut_wait_child_at(__FILE__, __LINE__, (child))
#define UT_EXPECT_EXIT(child, code)                                            \
	ut_expect_exit_at(__FILE__, __LINE__, (child), (code))
#define UT_EXPECT_SIGNAL(child, signal)                                        \
	ut_expect_signal_at(__FILE__, __LINE__, (child), (signal))

#endif
