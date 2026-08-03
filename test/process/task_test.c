#include <kernel/test.h>
#include <kernel/errno.h>
#include <kernel/exit.h>
#include <kernel/task.h>
#include <kernel/cpu.h>
#include <kernel/pid.h>
#include <kernel/sched.h>
#include <kernel/signal.h>
#include <uapi/wait.h>

int test_task_layout_contract(void)
{
	TEST_BEGIN("task: layout contract");
	{
		TEST_ASSERT(arch_task_test_layout_contract());
	}
	TEST_END("task: layout contract");
	return __test_ret;
fail:
	TEST_FAIL("task: layout contract", "see above");

	return __test_ret;
}

int test_cpu_boot_topology(void)
{
	TEST_BEGIN("cpu: boot topology");
	{
		TEST_ASSERT_EQ((uint32_t)NR_CPUS, (uint32_t)CONFIG_QEMU_CPUS);
		TEST_ASSERT_EQ(nr_cpu_ids, (uint32_t)1);
		TEST_ASSERT_EQ(current_cpu(), cpu_by_id(0));
		TEST_ASSERT(cpu_is_online(0));

		for (uint32_t id = 1; id < NR_CPUS; id++)
			TEST_ASSERT(!cpu_is_online(id));
	}
	TEST_END("cpu: boot topology");
	return __test_ret;
fail:
	TEST_FAIL("cpu: boot topology", "see above");

	return __test_ret;
}

int test_cpu_current_task_accessors(void)
{
	struct task_struct *saved = current_task();
	struct task_struct *task = NULL;

	TEST_BEGIN("cpu: active task accessors");
	{
		TEST_ASSERT_EQ(cpu_current_task(current_cpu()), saved);

		task = task_alloc();
		TEST_ASSERT_NOT_NULL(task);

		set_current_task(task);
		TEST_ASSERT_EQ(current_task(), task);
		TEST_ASSERT_EQ(cpu_current_task(current_cpu()), task);
	}
	TEST_END("cpu: active task accessors");
	goto cleanup;
fail:
	TEST_FAIL("cpu: active task accessors", "see above");
cleanup:
	set_current_task(saved);
	if (task)
		task_free(task);

	return __test_ret;
}

int test_task_alloc_free(void)
{
	TEST_BEGIN("task: alloc/free");
	{
		struct task_struct *task = task_alloc();
		TEST_ASSERT_NOT_NULL(task);

		TEST_ASSERT(task_pid(task) > 0);
		TEST_ASSERT(task_pid(task) <= PID_MAX);

		TEST_ASSERT_EQ(task_state(task), (uint32_t)TASK_RUNNING);

		TEST_ASSERT_NOT_NULL(task_kernel_stack(task));

		TEST_ASSERT_ALIGNED(task_kernel_stack(task), PAGE_SIZE);

		TEST_ASSERT_NULL(task_mm(task));

		TEST_ASSERT_NULL(task_trap_frame(task));

		TEST_ASSERT_EQ(task_child_count(task), (uint32_t)0);
		TEST_ASSERT_NULL(task_parent(task));
		TEST_ASSERT(!task_is_queued(task));

		task_free(task);
	}
	TEST_END("task: alloc/free");
	return __test_ret;
fail:
	TEST_FAIL("task: alloc/free", "see above");

	return __test_ret;
}

int test_task_multiple(void)
{
	TEST_BEGIN("task: multiple tasks");
	{
#define TASK_N_TASKS 8
		struct task_struct *tasks[TASK_N_TASKS];
		pid_t pids[TASK_N_TASKS];

		for (int i = 0; i < TASK_N_TASKS; i++) {
			tasks[i] = task_alloc();
			TEST_ASSERT_NOT_NULL(tasks[i]);
			pids[i] = task_pid(tasks[i]);
		}

		for (int i = 0; i < TASK_N_TASKS; i++) {
			for (int j = i + 1; j < TASK_N_TASKS; j++) {
				TEST_ASSERT_NE(pids[i], pids[j]);
			}
		}

		for (int i = 0; i < TASK_N_TASKS; i++)
			task_free(tasks[i]);
#undef TASK_N_TASKS
	}
	TEST_END("task: multiple tasks");
	return __test_ret;
fail:
	TEST_FAIL("task: multiple tasks", "see above");

	return __test_ret;
}

int test_task_process_tree(void)
{
	TEST_BEGIN("task: process tree linkage");
	{
		struct task_struct *parent = task_alloc();
		TEST_ASSERT_NOT_NULL(parent);

		struct task_struct *child1 = task_alloc();
		TEST_ASSERT_NOT_NULL(child1);

		struct task_struct *child2 = task_alloc();
		TEST_ASSERT_NOT_NULL(child2);

		task_link_child(parent, child1);

		task_link_child(parent, child2);

		TEST_ASSERT_EQ(task_child_count(parent), (uint32_t)2);

		task_unlink_child(child1);
		task_unlink_child(child2);
		task_free(child2);
		task_free(child1);
		task_free(parent);
	}
	TEST_END("task: process tree linkage");
	return __test_ret;
fail:
	TEST_FAIL("task: process tree linkage", "see above");

	return __test_ret;
}

int test_task_idle(void)
{
	TEST_BEGIN("task: idle task init");
	{
		TEST_ASSERT_NOT_NULL(current_task());
		TEST_ASSERT_EQ(task_pid(&idle_task), (pid_t)0);
		TEST_ASSERT_EQ(task_state(&idle_task), (uint32_t)TASK_RUNNING);
	}
	TEST_END("task: idle task init");
	return __test_ret;
fail:
	TEST_FAIL("task: idle task init", "see above");

	return __test_ret;
}

int test_task_free_null(void)
{
	TEST_BEGIN("task: free(NULL) safe");
	{
		task_free(NULL);
	}
	TEST_END("task: free(NULL) safe");

	return __test_ret;
}

int test_task_publish_lookup_lifetime(void)
{
	struct task_struct *task = NULL;
	struct task_struct *lookup = NULL;
	pid_t pid = -1;
	bool published = false;

	TEST_BEGIN("task: publish lookup lifetime");
	{
		task = task_alloc();
		TEST_ASSERT_NOT_NULL(task);
		pid = task_pid(task);
		TEST_ASSERT_NULL(task_find_thread(pid));

		task_publish(task);
		published = true;
		lookup = task_find_thread(pid);
		TEST_ASSERT_EQ(lookup, task);

		task_unpublish(task);
		published = false;
		TEST_ASSERT_NULL(task_find_thread(pid));
	}
	TEST_END("task: publish lookup lifetime");
	goto cleanup;
fail:
	TEST_FAIL("task: publish lookup lifetime", "see above");
cleanup:
	if (lookup)
		task_put(lookup);
	if (task) {
		if (published)
			task_unpublish(task);
		task_put(task);
	}

	return __test_ret;
}

int test_task_try_get_lifetime(void)
{
	struct task_struct *task = NULL;
	int refs;

	TEST_BEGIN("task: try-get lifetime");
	{
		task = task_alloc();
		TEST_ASSERT_NOT_NULL(task);
		TEST_ASSERT(!task_try_get(NULL));
		refs = refcount_read(&task->lifecycle.refs);
		TEST_ASSERT(task_try_get(task));
		TEST_ASSERT_EQ(refcount_read(&task->lifecycle.refs), refs + 1);
		task_put(task);
		TEST_ASSERT_EQ(refcount_read(&task->lifecycle.refs), refs);
		TEST_ASSERT(task_try_get(&idle_task));
		TEST_ASSERT_EQ(refcount_read(&idle_task.lifecycle.refs), 1);
	}
	TEST_END("task: try-get lifetime");
	if (task)
		task_free(task);
	return __test_ret;
fail:
	TEST_FAIL("task: try-get lifetime", "see above");
	if (task && refcount_read(&task->lifecycle.refs) > 1)
		task_put(task);
	if (task)
		task_free(task);
	return __test_ret;
}

int test_wait4_stop_continue_events(void)
{
	struct task_struct *saved = current_task();
	struct task_struct *parent = NULL;
	struct task_struct *child = NULL;
	struct wait4_result result = {0};
	pid_t child_pid = -1;
	uint64_t first_sequence;

	TEST_BEGIN("wait4: stop and continue events");
	{
		parent = task_alloc();
		TEST_ASSERT_NOT_NULL(parent);
		child = task_alloc();
		TEST_ASSERT_NOT_NULL(child);
		child_pid = task_pid(child);
		task_link_child(parent, child);
		set_current_task(parent);

		TEST_ASSERT_EQ(kernel_wait4(-1, WNOHANG, &result), 0);
		TEST_ASSERT_EQ(result.pid, (pid_t)0);
		TEST_ASSERT_EQ(kernel_wait4(0, WNOHANG, &result), -EINVAL);
		TEST_ASSERT_EQ(kernel_wait4(-2, WNOHANG, &result), -EINVAL);
		TEST_ASSERT_EQ(kernel_wait4(-1, 0x4, &result), -EINVAL);
		TEST_ASSERT_EQ(send_signal(SIGUSR1, parent), 0);
		TEST_ASSERT_EQ(kernel_wait4(-1, 0, &result), -EINTR);
		signal_clear_pending(parent, signal_mask(SIGUSR1));
		TEST_ASSERT_EQ(send_signal(SIGTSTP, child), 0);
		TEST_ASSERT(task_pending_mask(child) & signal_mask(SIGTSTP));
		TEST_ASSERT_EQ(send_signal(SIGCONT, child), 0);
		TEST_ASSERT_EQ(task_pending_mask(child) & signal_mask(SIGTSTP),
			       (uint64_t)0);
		signal_clear_pending(child, signal_mask(SIGCONT));

		task_mark_stopped(child);
		task_child_publish_stop(child, SIGTSTP);
		TEST_ASSERT_EQ(send_signal(SIGCONT, child), 0);
		task_mark_stopped(child);
		task_child_publish_stop(child, SIGSTOP);

		TEST_ASSERT_EQ(
			kernel_wait4(-1, WUNTRACED | WCONTINUED, &result), 0);
		TEST_ASSERT_EQ(result.pid, child_pid);
		TEST_ASSERT_EQ(result.status, (SIGTSTP << 8) | 0x7f);
		TEST_ASSERT_EQ(result.claim.type, TASK_CHILD_EVENT_STOP);
		first_sequence = result.claim.sequence;
		kernel_wait4_abort(&result);

		TEST_ASSERT_EQ(
			kernel_wait4(-1, WUNTRACED | WCONTINUED, &result), 0);
		TEST_ASSERT_EQ(result.claim.sequence, first_sequence);
		kernel_wait4_finish(&result);

		TEST_ASSERT_EQ(kernel_wait4(child_pid, WUNTRACED | WCONTINUED,
					    &result),
			       0);
		TEST_ASSERT_EQ(result.status, 0xffff);
		TEST_ASSERT_EQ(result.claim.type, TASK_CHILD_EVENT_CONTINUE);
		kernel_wait4_finish(&result);

		TEST_ASSERT_EQ(kernel_wait4(child_pid, WUNTRACED | WCONTINUED,
					    &result),
			       0);
		TEST_ASSERT_EQ(result.status, (SIGSTOP << 8) | 0x7f);
		TEST_ASSERT_EQ(result.claim.type, TASK_CHILD_EVENT_STOP);
		kernel_wait4_finish(&result);

		TEST_ASSERT_EQ(kernel_wait4(-1, WNOHANG | WCONTINUED, &result),
			       0);
		TEST_ASSERT_EQ(result.pid, (pid_t)0);
	}
	TEST_END("wait4: stop and continue events");
	goto cleanup;
fail:
	TEST_FAIL("wait4: stop and continue events", "see above");
cleanup:
	set_current_task(saved);
	if (child) {
		if (task_is_queued(child))
			sched_dequeue(child);
		task_unlink_child(child);
		task_free(child);
	}
	if (parent)
		task_free(parent);

	return __test_ret;
}

int test_wait4_signal_exit_status(void)
{
	struct task_struct *saved = current_task();
	struct task_struct *parent = NULL;
	struct task_struct *child = NULL;
	struct wait4_result result = {0};
	pid_t child_pid = -1;

	TEST_BEGIN("wait4: signal termination status");
	{
		parent = task_alloc();
		TEST_ASSERT_NOT_NULL(parent);
		child = task_alloc();
		TEST_ASSERT_NOT_NULL(child);
		child_pid = task_pid(child);
		task_link_child(parent, child);
		task_publish(child);
		task_child_publish_exit(child, SIGTERM);
		set_current_task(parent);

		TEST_ASSERT_EQ(kernel_wait4(child_pid, 0, &result), 0);
		TEST_ASSERT_EQ(result.pid, child_pid);
		TEST_ASSERT_EQ(result.status, SIGTERM);
		TEST_ASSERT_EQ(result.claim.type, TASK_CHILD_EVENT_EXIT);
		kernel_wait4_finish(&result);
		child = NULL;
	}
	TEST_END("wait4: signal termination status");
	goto cleanup;
fail:
	TEST_FAIL("wait4: signal termination status", "see above");
cleanup:
	set_current_task(saved);
	if (child) {
		task_unlink_child(child);
		task_unpublish(child);
		task_put(child);
	}
	if (parent)
		task_free(parent);

	return __test_ret;
}
