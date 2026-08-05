/*
 * kernel/exit.c - 进程退出与回收
 */

#include <kernel/exit.h>
#include <kernel/errno.h>
#include <kernel/fdtable.h>
#include <kernel/fork.h>
#include <kernel/futex.h>
#include <kernel/fs_struct.h>
#include <kernel/list.h>
#include <kernel/mm.h>
#include <kernel/printk.h>
#include <kernel/resource.h>
#include <kernel/sched.h>
#include <kernel/session.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/syscall.h>
#include <kernel/wait.h>
#include <kernel/processor.h>
#include <kernel/pgtable.h>
#include <uapi/wait.h>

#define WEXITCODE(code) (((code) & 0xff) << 8)

struct wait4_selector {
	pid_t pid;
	uint32_t event_mask;
};

LIST_HEAD_STATIC(exited_threads);
static bool exited_threads_reap_pending;

static uint32_t wait4_event_mask(int options)
{
	uint32_t event_mask = TASK_CHILD_EVENT_MASK_EXIT;

	if (options & WUNTRACED)
		event_mask |= TASK_CHILD_EVENT_MASK_STOP;
	if (options & WCONTINUED)
		event_mask |= TASK_CHILD_EVENT_MASK_CONTINUE;
	return event_mask;
}

static int wait4_probe(struct wait_session *session, void *arg)
{
	const struct wait4_selector *selector = arg;

	return task_child_event_watch(session, current_task(), selector->pid,
				      selector->event_mask);
}

static void clear_child_tid(struct task_struct *task)
{
	int *clear_tid = task_clear_child_tid(task);
	int zero = 0;

	if (!clear_tid)
		return;

	if (copy_to_user(clear_tid, &zero, sizeof(zero)) == 0 &&
	    futex_wake_mm(task_mm(task), clear_tid, 1) < 0)
		pr_debug("futex: clear_child_tid wake failed\n");
	task_set_clear_child_tid(task, NULL);
}

static void release_task_mm(struct task_struct *task)
{
	struct mm_struct *mm = task_mm(task);

	if (!mm)
		return;

	task_set_mm(task, NULL);
	task_set_satp(task, 0);

	if (task == current_task())
		pgtable_activate_kernel();

	mm_put(mm);
}

static void detach_task_queues(struct task_struct *task)
{
	if (!task || task == current_task())
		return;

	if (!list_empty(&task->sched.run_list))
		sched_dequeue(task);
}

__nonnull(1)
static void finish_task_exit(struct task_struct *task, int status)
{
	bool leader;

	if (task_state(task) == TASK_ZOMBIE || task_state(task) == TASK_DEAD)
		return;

	detach_task_queues(task);
	/* Resource teardown follows the waiter's synchronous cancellation. */
	wait_cancel_task(task);
	restart_clear(task);

	task_set_exit_code(task, status);
	futex_exit_robust_list(task);
	clear_child_tid(task);
	close_files(task);
	exit_fs(task);
	session_process_exit(task);
	signals_release(task);
	release_task_mm(task);
	kernel_clone_complete_vfork(task);

	leader = task_is_group_leader(task);
	if (leader) {
		task_child_publish_exit(task, status);
	} else {
		if (!list_empty(&task->links.thread_node))
			list_del_init(&task->links.thread_node);
		task_set_state(task, TASK_ZOMBIE);
	}
	if (!leader && task == current_task()) {
		list_add_tail(&task->links.thread_node, &exited_threads);
		exited_threads_reap_pending = true;
	}
}

void reap_exited_threads(void)
{
	struct list_head *pos;
	struct list_head *next;

	if (!exited_threads_reap_pending)
		return;

	list_for_each_safe (pos, next, &exited_threads) {
		struct task_struct *thread =
			list_entry(pos, struct task_struct, links.thread_node);

		if (thread == current_task())
			continue;

		release_task(thread);
	}

	if (list_empty(&exited_threads))
		exited_threads_reap_pending = false;
}

static void reap_other_threads(struct task_struct *leader, int status)
{
	struct list_head *pos;
	struct list_head *next;

	list_for_each_safe (pos, next, &leader->links.thread_group) {
		struct task_struct *thread =
			list_entry(pos, struct task_struct, links.thread_node);

		if (thread == current_task())
			continue;

		finish_task_exit(thread, status);
		release_task(thread);
	}
}

__noreturn
static void exit_schedule(void)
{
	schedule();

	unreachable();
}

__noreturn
static void do_exit_status(int status)
{
	struct task_struct *task = current_task();

	BUG_ON(!task);
	if (!task_is_group_leader(task)) {
		finish_task_exit(task, status);
	} else {
		reap_other_threads(task, status);
		finish_task_exit(task, status);
	}

	exit_schedule();
}

__noreturn
void do_exit(int code)
{
	do_exit_status(WEXITCODE(code));
}

__noreturn
void do_exit_group(int code)
{
	struct task_struct *task = current_task();
	struct task_struct *leader;
	int status = WEXITCODE(code);

	BUG_ON(!task);
	leader = task_group_leader(task);

	if (leader && leader != task)
		finish_task_exit(leader, status);

	if (leader)
		reap_other_threads(leader, status);

	finish_task_exit(task, status);
	exit_schedule();
}

__noreturn
void do_exit_signal(int sig)
{
	int status = sig & 0x7f;

	BUG_ON(status == 0);
	do_exit_status(status);
}

void release_task(struct task_struct *task)
{
	if (!task)
		return;

	BUG_ON(task == current_task());
	BUG_ON(task == &idle_task);
	task_child_release_zombie(task);

	if (!list_empty(&task->links.thread_node))
		list_del_init(&task->links.thread_node);
	if (!list_empty(&task->sched.run_list))
		sched_dequeue(task);
	task_unpublish(task);
	task_put(task);
}

int kernel_wait4(pid_t pid, int options, struct wait4_result *result)
{
	const struct wait_deadline deadline = {
		.active = false,
	};
	struct wait4_selector selector = {
		.pid = pid,
		.event_mask = wait4_event_mask(options),
	};
	struct wait_request source = {
		.kind = WAIT_KIND_CHILD,
		.check = wait4_probe,
		.arg = &selector,
		.channel_limit = 1,
	};
	wait_outcome_t outcome;

	if (pid != (pid_t)-1 && pid <= 0)
		return -EINVAL;
	if (options & ~(WNOHANG | WUNTRACED | WCONTINUED))
		return -EINVAL;
	if (!result)
		return -EINVAL;
	memset(result, 0, sizeof(*result));

	while (true) {
		enum task_child_wait_state state;

		state = task_child_event_claim_next(current_task(), pid,
						    selector.event_mask,
						    &result->claim);
		if (state == TASK_CHILD_WAIT_NO_CHILD)
			return -ECHILD;

		if (state == TASK_CHILD_WAIT_NO_EVENT) {
			if (options & WNOHANG)
				return 0;

			int ret = wait_for_interruptible(&source, &deadline, &outcome);
			if (ret < 0)
				return ret;
			if (outcome == WAIT_OUTCOME_SIGNAL)
				return -EINTR;
			BUG_ON(outcome != WAIT_OUTCOME_EVENT);
			continue;
		}

		BUG_ON(state != TASK_CHILD_WAIT_EVENT);
		result->pid = result->claim.pid;
		result->status = result->claim.status;
		task_cputime_total(result->claim.child, &result->cputime);
		return 0;
	}
}

void kernel_wait4_finish(struct wait4_result *result)
{
	if (!result || !result->claim.child)
		return;

	BUG_ON(!task_child_event_commit(&result->claim));
	if (result->claim.type == TASK_CHILD_EVENT_EXIT) {
		task_add_child_time(current_task(), &result->cputime);
		release_task(result->claim.child);
	}
	memset(result, 0, sizeof(*result));
}

void kernel_wait4_abort(struct wait4_result *result)
{
	if (!result || !result->claim.child)
		return;

	task_child_event_abort(&result->claim);
	memset(result, 0, sizeof(*result));
}
