#ifndef _NUVIX_UTEST_INTERNAL_H
#define _NUVIX_UTEST_INTERNAL_H

#include <stdint.h>

#include <utest.h>

enum ut_case_status {
	UT_CASE_PASS,
	UT_CASE_FAIL,
	UT_CASE_SKIP,
	UT_CASE_XFAIL,
	UT_CASE_XPASS,
};

#define UT_REASON_SIZE 160

struct ut_case_result {
	enum ut_case_status status;
	unsigned int failures;
	char reason[UT_REASON_SIZE];
};

void ut_run_case(const struct ut_case *test_case, const char *fixture,
		 struct ut_case_result *result);
int ut_cleanup_fixture(const char *fixture);

#endif
