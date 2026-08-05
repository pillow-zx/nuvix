#include <kernel/errno.h>
#include <kernel/test.h>
#include <kernel/time.h>
#include <kernel/timer.h>

#include "../ktest.h"

static void ktimer_test_callback(struct ktimer *timer, void *arg)
{
	int *count = arg;

	(void)timer;
	(*count)++;
}

static void ktimer_self_cancel_callback(struct ktimer *timer, void *arg)
{
	int *ret = arg;

	*ret = ktimer_cancel_sync(timer);
}

int test_ktimer_cancel_sync(void)
{
	struct ktimer timer;
	int self_cancel_ret = 0;

	TEST_BEGIN("ktimer: synchronous cancel contract");
	{
		ktimer_init(&timer, ktimer_test_callback, NULL);
		TEST_ASSERT_EQ(ktimer_arm(&timer, 100, 0), 0);
		local_irq_disable();
		TEST_ASSERT_EQ(ktimer_cancel_sync(&timer), 0);
		TEST_ASSERT(irqs_disabled());
		local_irq_enable();

		ktimer_init(&timer, ktimer_self_cancel_callback, &self_cancel_ret);
		TEST_ASSERT_EQ(ktimer_arm(&timer, 100, 0), 0);
		ktimer_run_expired(100);
		TEST_ASSERT_EQ(self_cancel_ret, -EDEADLK);
		TEST_ASSERT(!ktimer_active(&timer));
	}
	TEST_END("ktimer: synchronous cancel contract");
	return __test_ret;
fail:
	if (ktimer_active(&timer)) {
		bool cancelled = ktimer_cancel(&timer);

		(void)cancelled;
	}
	TEST_FAIL("ktimer: synchronous cancel contract", "see above");
	return __test_ret;
}

int test_ktimer_arm_cancel_remaining(void)
{
	struct ktimer timer;
	int fired = 0;

	TEST_BEGIN("ktimer: arm cancel remaining");
	{
		ktimer_init(&timer, ktimer_test_callback, &fired);
		TEST_ASSERT(!ktimer_active(&timer));
		TEST_ASSERT_EQ(ktimer_remaining(&timer, 10), (uint64_t)0);

		TEST_ASSERT_EQ(ktimer_arm(&timer, 100, 0), 0);
		TEST_ASSERT(ktimer_active(&timer));
		TEST_ASSERT_EQ(ktimer_remaining(&timer, 40), (uint64_t)60);

		TEST_ASSERT(ktimer_cancel(&timer));
		TEST_ASSERT(!ktimer_active(&timer));
		TEST_ASSERT_EQ(ktimer_remaining(&timer, 40), (uint64_t)0);

		ktimer_run_expired(100);
		TEST_ASSERT_EQ(fired, 0);
	}
	TEST_END("ktimer: arm cancel remaining");
	return __test_ret;
fail:
	if (ktimer_active(&timer)) {
		bool cancelled = ktimer_cancel(&timer);

		(void)cancelled;
	}
	TEST_FAIL("ktimer: arm cancel remaining", "see above");

	return __test_ret;
}

int test_ktimer_timer_run_expired_callback(void)
{
	struct ktimer timer;
	int fired = 0;

	TEST_BEGIN("ktimer: timer_run_expired callback");
	{
		ktimer_init(&timer, ktimer_test_callback, &fired);
		TEST_ASSERT_EQ(ktimer_arm(&timer, 100, 0), 0);

		timer_run_expired(99);
		TEST_ASSERT_EQ(fired, 0);
		TEST_ASSERT(ktimer_active(&timer));

		timer_run_expired(100);
		TEST_ASSERT_EQ(fired, 1);
		TEST_ASSERT(!ktimer_active(&timer));
	}
	TEST_END("ktimer: timer_run_expired callback");
	return __test_ret;
fail:
	if (ktimer_active(&timer)) {
		bool cancelled = ktimer_cancel(&timer);

		(void)cancelled;
	}
	TEST_FAIL("ktimer: timer_run_expired callback", "see above");

	return __test_ret;
}

int test_ktimer_interval_rearms_after_expiry(void)
{
	struct ktimer timer;
	int fired = 0;

	TEST_BEGIN("ktimer: interval rearms after expiry");
	{
		ktimer_init(&timer, ktimer_test_callback, &fired);
		TEST_ASSERT_EQ(ktimer_arm(&timer, 100, 25), 0);

		timer_run_expired(100);
		TEST_ASSERT_EQ(fired, 1);
		TEST_ASSERT(ktimer_active(&timer));
		TEST_ASSERT_EQ(ktimer_remaining(&timer, 100), (uint64_t)25);

		timer_run_expired(124);
		TEST_ASSERT_EQ(fired, 1);
		TEST_ASSERT(ktimer_active(&timer));

		timer_run_expired(125);
		TEST_ASSERT_EQ(fired, 2);
		TEST_ASSERT(ktimer_active(&timer));
		TEST_ASSERT_EQ(ktimer_remaining(&timer, 125), (uint64_t)25);

		TEST_ASSERT(ktimer_cancel(&timer));
	}
	TEST_END("ktimer: interval rearms after expiry");
	return __test_ret;
fail:
	if (ktimer_active(&timer)) {
		bool cancelled = ktimer_cancel(&timer);

		(void)cancelled;
	}
	TEST_FAIL("ktimer: interval rearms after expiry", "see above");

	return __test_ret;
}
