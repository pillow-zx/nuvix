#include <kernel/errno.h>
#include <kernel/exit.h>
#include <kernel/signal.h>
#include <kernel/sched.h>
#include <kernel/task.h>
#include <kernel/test.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#include "../ktest.h"

struct wait_test_source {
	struct wait_channel *first;
	struct wait_channel *second;
	struct wait_deadline *deadline;
	uint32_t probes;
	uint32_t ready_probe;
	uint32_t wake_probe;
	uint32_t timeout_probe;
	bool duplicate_first;
};

struct wait_cancel_test {
	struct wait_channel channel;
	volatile bool started;
	volatile bool resumed;
	int cancel_count;
};

static int wait_cancel_probe(struct wait_session *session, void *arg)
{
	struct wait_cancel_test *test = arg;

	test->started = true;
	return wait_session_watch(session, &test->channel);
}

static void wait_cancel_callback(void *arg)
{
	struct wait_cancel_test *test = arg;

	test->cancel_count++;
}

static void wait_cancel_target(void *arg)
{
	struct wait_cancel_test *test = arg;
	const struct wait_deadline deadline = wait_deadline_none();
	const struct wait_request source = {
		.kind = WAIT_KIND_GENERIC,
		.check = wait_cancel_probe,
		.cancel = wait_cancel_callback,
		.arg = test,
		.channel_limit = 1,
	};
	wait_outcome_t outcome;
	int ret;

	ret = wait_for(&source, WAIT_FLAG_INTERRUPTIBLE, &deadline, &outcome);
	(void)ret;
	test->resumed = true;
	do_exit(1);
}

static int wait_test_probe(struct wait_session *session, void *arg)
{
	struct wait_test_source *source = arg;
	int ret;

	source->probes++;
	if (source->first) {
		ret = wait_session_watch(session, source->first);
		if (ret < 0)
			return ret;
		if (source->duplicate_first) {
			ret = wait_session_watch(session, source->first);
			if (ret < 0)
				return ret;
		}
	}
	if (source->second) {
		ret = wait_session_watch(session, source->second);
		if (ret < 0)
			return ret;
	}
	if (source->wake_probe == source->probes)
		wait_channel_wake_one(source->first);
	if (source->timeout_probe == source->probes)
		source->deadline->expires = timer_now();
	return source->ready_probe == source->probes;
}

static int wait_test_error_probe(struct wait_session *session, void *arg)
{
	struct wait_channel **queues = arg;
	int ret;

	ret = wait_session_watch(session, queues[0]);
	if (ret < 0)
		return ret;
	return wait_session_watch(session, queues[1]);
}

int test_wait_for_timeout(void)
{
	struct wait_deadline deadline = wait_deadline_at(timer_now());
	wait_outcome_t outcome = 99;

	TEST_BEGIN("wait outcome: timeout");
	{
		TEST_ASSERT_EQ(wait_for(NULL, 0, &deadline, &outcome), 0);
		TEST_ASSERT_EQ(outcome,
			       (wait_outcome_t)WAIT_OUTCOME_TIMEOUT);
		TEST_ASSERT_EQ(task_state(current_task()),
			       (uint32_t)TASK_RUNNING);
	}
	TEST_END("wait outcome: timeout");
	return __test_ret;
fail:
	TEST_FAIL("wait outcome: timeout", "see above");
	return __test_ret;
}

int test_wait_for_event(void)
{
	struct wait_channel channel;
	struct wait_deadline deadline = wait_deadline_none();
	struct wait_test_source test_source = { 0 };
	struct wait_request source;
	wait_outcome_t outcome = 0;

	TEST_BEGIN("wait outcome: event");
	{
		wait_channel_init(&channel);
		test_source.first = &channel;
		test_source.ready_probe = 1;
		source = (struct wait_request){
			.kind = WAIT_KIND_GENERIC,
			.check = wait_test_probe,
			.arg = &test_source,
			.channel_limit = 1,
		};
		TEST_ASSERT_EQ(wait_for(&source, 0, &deadline, &outcome),
			       0);
		TEST_ASSERT_EQ(outcome,
			       (wait_outcome_t)WAIT_OUTCOME_EVENT);
		TEST_ASSERT(list_empty(&channel.waiters));
		TEST_ASSERT_EQ(task_state(current_task()),
			       (uint32_t)TASK_RUNNING);
	}
	TEST_END("wait outcome: event");
	return __test_ret;
fail:
	TEST_FAIL("wait outcome: event", "see above");
	return __test_ret;
}

int test_wait_for_spurious_retry(void)
{
	struct wait_channel channel;
	struct wait_deadline deadline = wait_deadline_at(UINT64_MAX);
	struct wait_test_source test_source = { 0 };
	struct wait_request source;
	wait_outcome_t outcome = 0;

	TEST_BEGIN("wait outcome: spurious retry");
	{
		wait_channel_init(&channel);
		test_source.first = &channel;
		test_source.deadline = &deadline;
		test_source.wake_probe = 2;
		test_source.timeout_probe = 3;
		source = (struct wait_request){
			.kind = WAIT_KIND_GENERIC,
			.check = wait_test_probe,
			.arg = &test_source,
			.channel_limit = 1,
		};
		TEST_ASSERT_EQ(wait_for(&source, 0, &deadline, &outcome),
			       0);
		TEST_ASSERT_EQ(outcome,
			       (wait_outcome_t)WAIT_OUTCOME_TIMEOUT);
		TEST_ASSERT_EQ(test_source.probes, (uint32_t)3);
		TEST_ASSERT(list_empty(&channel.waiters));
		TEST_ASSERT_EQ(task_state(current_task()),
			       (uint32_t)TASK_RUNNING);
	}
	TEST_END("wait outcome: spurious retry");
	return __test_ret;
fail:
	TEST_FAIL("wait outcome: spurious retry", "see above");
	return __test_ret;
}

int test_wait_for_priority(void)
{
	struct wait_channel channel;
	struct wait_deadline deadline = wait_deadline_at(timer_now());
	struct wait_test_source test_source = { 0 };
	struct wait_request source;
	uint64_t saved_pending = current_task()->sigctx.pending;
	uint64_t saved_blocked = current_task()->sigctx.blocked;
	wait_outcome_t outcome = 0;

	TEST_BEGIN("wait outcome: priority");
	{
		wait_channel_init(&channel);
		current_task()->sigctx.blocked &= ~signal_mask(SIGUSR1);
		TEST_ASSERT_EQ(send_current_signal(SIGUSR1), 0);
		test_source.first = &channel;
		test_source.ready_probe = 1;
			source = (struct wait_request){
			.kind = WAIT_KIND_GENERIC,
			.check = wait_test_probe,
			.arg = &test_source,
			.channel_limit = 1,
		};
		TEST_ASSERT_EQ(wait_for(&source, WAIT_FLAG_INTERRUPTIBLE,
					     &deadline, &outcome),
			       0);
		TEST_ASSERT_EQ(outcome,
			       (wait_outcome_t)WAIT_OUTCOME_EVENT);

		test_source.ready_probe = 0;
		test_source.probes = 0;
		outcome = 0;
		TEST_ASSERT_EQ(wait_for(&source, WAIT_FLAG_INTERRUPTIBLE,
					     &deadline, &outcome),
			       0);
		TEST_ASSERT_EQ(outcome,
			       (wait_outcome_t)WAIT_OUTCOME_SIGNAL);
		TEST_ASSERT(list_empty(&channel.waiters));
	}
	TEST_END("wait outcome: priority");
	goto cleanup;
fail:
	TEST_FAIL("wait outcome: priority", "see above");
cleanup:
	current_task()->sigctx.pending = saved_pending;
	current_task()->sigctx.blocked = saved_blocked;
	return __test_ret;
}

int test_wait_for_wake_before_block(void)
{
	struct wait_channel channel;
	struct wait_deadline deadline = wait_deadline_none();
	struct wait_test_source test_source = { 0 };
	struct wait_request source;
	wait_outcome_t outcome = 0;

	TEST_BEGIN("wait outcome: wake before block");
	{
		wait_channel_init(&channel);
		test_source.first = &channel;
		test_source.wake_probe = 2;
		test_source.ready_probe = 3;
		source = (struct wait_request){
			.kind = WAIT_KIND_GENERIC,
			.check = wait_test_probe,
			.arg = &test_source,
			.channel_limit = 1,
		};
		TEST_ASSERT_EQ(wait_for(&source, 0, &deadline, &outcome),
			       0);
		TEST_ASSERT_EQ(outcome,
			       (wait_outcome_t)WAIT_OUTCOME_EVENT);
		TEST_ASSERT_EQ(test_source.probes, (uint32_t)3);
		TEST_ASSERT(list_empty(&channel.waiters));
	}
	TEST_END("wait outcome: wake before block");
	return __test_ret;
fail:
	TEST_FAIL("wait outcome: wake before block", "see above");
	return __test_ret;
}

int test_wait_for_registration(void)
{
	struct wait_channel first;
	struct wait_channel second;
	struct wait_deadline deadline = wait_deadline_none();
	struct wait_test_source test_source = { 0 };
	struct wait_request source;
	wait_outcome_t outcome = 0;
	int refs = refcount_read(&current_task()->lifecycle.refs);

	TEST_BEGIN("wait outcome: registration");
	{
		wait_channel_init(&first);
		wait_channel_init(&second);
		test_source.first = &first;
		test_source.second = &second;
		test_source.duplicate_first = true;
		test_source.ready_probe = 1;
		source = (struct wait_request){
			.kind = WAIT_KIND_GENERIC,
			.check = wait_test_probe,
			.arg = &test_source,
			.channel_limit = 2,
		};
		TEST_ASSERT_EQ(wait_for(&source, 0, &deadline, &outcome),
			       0);
		TEST_ASSERT_EQ(outcome,
			       (wait_outcome_t)WAIT_OUTCOME_EVENT);
		TEST_ASSERT(list_empty(&first.waiters));
		TEST_ASSERT(list_empty(&second.waiters));
		TEST_ASSERT_EQ(refcount_read(&current_task()->lifecycle.refs), refs);
	}
	TEST_END("wait outcome: registration");
	return __test_ret;
fail:
	TEST_FAIL("wait outcome: registration", "see above");
	return __test_ret;
}

int test_wait_cancel_callback(void)
{
	struct wait_cancel_test test = {0};
	struct task_struct *target = NULL;

	TEST_BEGIN("wait cancellation: adapter cleanup");
	{
		wait_channel_init(&test.channel);
		target = kernel_thread(wait_cancel_target, &test);
		TEST_ASSERT_NOT_NULL(target);
		for (int attempt = 0; attempt < 3 && !test.started; attempt++)
			schedule();
		TEST_ASSERT(test.started);
		TEST_ASSERT_EQ(task_state(target), (uint32_t)TASK_INTERRUPTIBLE);

		wait_cancel_task(target);
		TEST_ASSERT_EQ(test.cancel_count, 1);
		TEST_ASSERT(list_empty(&test.channel.waiters));
		TEST_ASSERT(!test.resumed);
		wait_cancel_task(target);
		TEST_ASSERT_EQ(test.cancel_count, 1);

		task_set_state(target, TASK_ZOMBIE);
		release_task(target);
		target = NULL;
	}
	TEST_END("wait cancellation: adapter cleanup");
	return __test_ret;
fail:
	TEST_FAIL("wait cancellation: adapter cleanup", "see above");
	if (target) {
		wait_cancel_task(target);
		task_set_state(target, TASK_ZOMBIE);
		release_task(target);
	}
	return __test_ret;
}

int test_wait_for_partial_error_cleanup(void)
{
	struct wait_channel first;
	struct wait_channel second;
	struct wait_channel *queues[2];
	struct wait_deadline deadline = wait_deadline_none();
	struct wait_request source;
	wait_outcome_t outcome = 99;

	TEST_BEGIN("wait outcome: partial error cleanup");
	{
		wait_channel_init(&first);
		wait_channel_init(&second);
		queues[0] = &first;
		queues[1] = &second;
		source = (struct wait_request){
			.kind = WAIT_KIND_GENERIC,
			.check = wait_test_error_probe,
			.arg = queues,
			.channel_limit = 1,
		};
		TEST_ASSERT_EQ(wait_for(&source, 0, &deadline, &outcome),
			       -E2BIG);
		TEST_ASSERT_EQ(outcome, (wait_outcome_t)0);
		TEST_ASSERT(list_empty(&first.waiters));
		TEST_ASSERT(list_empty(&second.waiters));
		TEST_ASSERT_EQ(task_state(current_task()),
			       (uint32_t)TASK_RUNNING);
	}
	TEST_END("wait outcome: partial error cleanup");
	return __test_ret;
fail:
	TEST_FAIL("wait outcome: partial error cleanup", "see above");
	return __test_ret;
}

int test_wait_for_signal_only(void)
{
	struct wait_deadline deadline = wait_deadline_none();
	uint64_t saved_pending = current_task()->sigctx.pending;
	uint64_t saved_blocked = current_task()->sigctx.blocked;
	wait_outcome_t outcome = 0;

	TEST_BEGIN("wait outcome: signal only");
	{
		current_task()->sigctx.blocked &= ~signal_mask(SIGUSR1);
		TEST_ASSERT_EQ(send_current_signal(SIGUSR1), 0);
		TEST_ASSERT_EQ(wait_for(NULL, WAIT_FLAG_INTERRUPTIBLE,
					     &deadline, &outcome),
			       0);
		TEST_ASSERT_EQ(outcome,
			       (wait_outcome_t)WAIT_OUTCOME_SIGNAL);
	}
	TEST_END("wait outcome: signal only");
	goto cleanup;
fail:
	TEST_FAIL("wait outcome: signal only", "see above");
cleanup:
	current_task()->sigctx.pending = saved_pending;
	current_task()->sigctx.blocked = saved_blocked;
	return __test_ret;
}

int test_wait_for_validation(void)
{
	struct wait_deadline no_deadline = wait_deadline_none();
	struct wait_deadline deadline = wait_deadline_at(timer_now());
	struct wait_request source = { .kind = WAIT_KIND_GENERIC };
	wait_outcome_t outcome = 99;

	TEST_BEGIN("wait outcome: validation");
	{
		outcome = 99;
		TEST_ASSERT_EQ(wait_for(NULL, 0, NULL, &outcome),
			       -EINVAL);
		TEST_ASSERT_EQ(outcome, (wait_outcome_t)0);
		TEST_ASSERT_EQ(wait_for(NULL, 0, &deadline, NULL),
			       -EINVAL);
		TEST_ASSERT_EQ(wait_for(NULL, WAIT_FLAG_MASK << 1, &deadline,
					     &outcome),
			       -EINVAL);
		TEST_ASSERT_EQ(outcome, (wait_outcome_t)0);
		outcome = 99;
		TEST_ASSERT_EQ(wait_for(NULL, 0, &no_deadline,
					     &outcome),
			       -EINVAL);
		TEST_ASSERT_EQ(outcome, (wait_outcome_t)0);
		source.channel_limit = 1;
		outcome = 99;
		TEST_ASSERT_EQ(wait_for(&source, 0, &deadline, &outcome),
			       -EINVAL);
		TEST_ASSERT_EQ(outcome, (wait_outcome_t)0);
		source.check = wait_test_probe;
		source.kind = WAIT_KIND_GENERIC;
		source.channel_limit = WAIT_SESSION_MAX_CHANNELS + 1;
		outcome = 99;
		TEST_ASSERT_EQ(wait_for(&source, 0, &deadline, &outcome),
			       -EINVAL);
		TEST_ASSERT_EQ(outcome, (wait_outcome_t)0);
		task_mark_interruptible_sleep(current_task());
		outcome = 99;
		TEST_ASSERT_EQ(wait_for(NULL, 0, &deadline, &outcome),
			       -EINVAL);
		TEST_ASSERT_EQ(outcome, (wait_outcome_t)0);
		TEST_ASSERT_EQ(task_state(current_task()),
			       (uint32_t)TASK_RUNNING);
	}
	TEST_END("wait outcome: validation");
	return __test_ret;
fail:
	TEST_FAIL("wait outcome: validation", "see above");
	return __test_ret;
}
