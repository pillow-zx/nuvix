/*
 * kernel/exit.c - task exit and proc-owned zombie/reap state
 */

#include <nuvix/errno.h>
#include <nuvix/exit.h>
#include <nuvix/fdtable.h>
#include <nuvix/fork.h>
#include <nuvix/mm.h>
#include <nuvix/pgtable.h>
#include <nuvix/printk.h>
#include <nuvix/proc.h>
#include <nuvix/processor.h>
#include <nuvix/resource.h>
#include <nuvix/sched.h>
#include <nuvix/signal.h>
#include <nuvix/session.h>
#include <nuvix/slab.h>
#include <nuvix/syscall.h>
#include <nuvix/task.h>
#include <nuvix/time.h>
#include <nuvix/timer.h>
#include <nuvix/wait.h>
#include <uapi/signal.h>
#include <uapi/wait.h>

#define WEXITCODE(code) (((code) & 0xff) << 8)

static uint32_t wait4_event_mask(int options)
{
	(void)options;
	return PROC_WAIT_EXIT;
}

static void account_task_to_proc(struct proc_struct *proc,
				 const struct task_struct *task)
{
	if (proc && task)
		proc_account_task_cputime(proc, &task->cputime);
}

/* All callers pass current_task(), which is never NULL in task context;
 * state the contract on the interface so the compiler can propagate it
 * without the analyzer modeling a NULL entry. */
__nonnull(1)
static void finish_task_exit(struct task_struct *task, int status)
{
	if (!task_begin_exit(task))
		return;
	wait_cancel_current();
	restart_clear(task);
	task_set_exit_code(task, status);
	proc_record_thread_exit(task, status);
	task_release_file_context(task);
	sig_task_release(task);
	task_mark_dead(task);
}

static void task_reaper_thread(void *arg)
{
	(void)arg;
	for (;;) {
		struct task_struct *task;

		mm_reap_retired();
		task_reap_deferred();
		while (sched_retired_pop(&task))
			release_task(task);
		sched_reaper_sleep();
	}
}

int task_reaper_start(void)
{
	return kernel_thread(task_reaper_thread, NULL) ? 0 : -ENOMEM;
}

__noreturn __cold
static void exit_schedule(void)
{
	sched_exit_current();
	panic("exit: scheduler returned for an exiting task");
	unreachable();
}

static void request_group_exit(struct task_struct *task, void *ignored)
{
	(void)ignored;
	(void)task_request_group_exit(task);
}

static void exit_group_status(struct proc_struct *proc, int status)
{
	if (!proc)
		return;
	(void)proc_begin_group_exit(proc, status);
	proc_notify_tasks(proc, request_group_exit, NULL);
	BUG_ON(!proc_group_exit_pending(proc, &status));
	finish_task_exit(current_task(), status);
}

__noreturn
static void do_exit_status(int status)
{
	struct task_struct *task = current_task();

	BUG_ON(!task);
	if (task->proc)
		exit_group_status(task->proc, status);
	else
		finish_task_exit(task, status);
	exit_schedule();
}

__noreturn
void do_exit(int code)
{
	struct task_struct *task = current_task();

	BUG_ON(!task);
	finish_task_exit(task, WEXITCODE(code));
	exit_schedule();
}

__noreturn
void do_exit_group(int code)
{
	do_exit_status(WEXITCODE(code));
}

__noreturn
void do_exit_signal(int sig)
{
	BUG_ON((sig & 0x7f) == 0);
	do_exit_status(sig & 0x7f);
}

void release_task(struct task_struct *task)
{
	struct proc_struct *proc = task->proc;
	struct proc_parent_event event;

	BUG_ON(!task_reap_ready(task));
	mm_put(task_replace_mm(task, NULL));
	kernel_clone_complete_vfork(task);
	if (proc) {
		bool last;

		account_task_to_proc(proc, task);
		task_reap_unpublish(task);
		last = proc_detach_task(proc, task);
		if (last) {
			struct proc_struct *parent = proc_parent_get(proc);
			struct sigchld_exit_policy policy = parent ?
				sigchld_exit_policy(parent) : (struct sigchld_exit_policy){0};
			bool sigchld = proc->wait_state.exit_signal == SIGCHLD;
			int status = proc->group_exit_status_valid ?
				proc->group_exit_status : proc->exit_status;

			session_process_exit(task);
			proc_prepare_exit(proc, status, task_uid(task),
					  sigchld && policy.auto_reap,
					  sigchld && policy.notify);
			proc_put(parent);
			proc_release_resources(proc);
			proc_publish_exit(proc, &event);
			sig_notify_parent(&event);
			proc_parent_event_release(&event);
			session_process_reaper(proc);
		}
	} else {
		task_reap_unpublish(task);
	}
	task_put(task);
}

static int wait_child(pid_t pid, int options, uint32_t events,
		      struct proc_wait_info *result)
{
	struct proc_struct *parent = current_task()->proc;
	struct proc_wait_selector selector;
	const struct wait_deadline deadline = wait_deadline_none();

	if (!parent)
		return -ECHILD;
	selector = (struct proc_wait_selector){
		.pid = pid, .creator_id = current_task()->identity,
		.child_class = options & __WALL ? PROC_WAIT_ALL_CHILDREN :
			options & __WCLONE ? PROC_WAIT_CLONE : PROC_WAIT_SIGCHLD,
		.creator_only = (options & __WNOTHREAD) != 0,
	};
	for (;;) {
		struct wait_scope scope __wait_scope = {};
		wait_outcome_t outcome;
		enum proc_wait_result state;
		int ret = wait_scope_begin(&scope, WAIT_FLAG_INTERRUPTIBLE, &deadline);

		if (ret < 0)
			return ret;
		ret = proc_wait_watch(parent, &selector, events, scope.wait);
		if (ret < 0)
			return ret;
		/* End registration before consumption can run final destructors. */
		if (ret > 0 || (options & WNOHANG)) {
			wait_scope_complete(&scope);
			state = proc_wait_take(parent, &selector, events,
					       !(options & WNOWAIT), result);
			if (state == PROC_WAIT_NO_CHILD)
				return -ECHILD;
			if (state == PROC_WAIT_EVENT || (options & WNOHANG))
				return 0;
			continue;
		}
		ret = wait_scope_block(&scope, &outcome);
		wait_scope_complete(&scope);
		if (ret < 0)
			return ret;
		if (outcome == WAIT_OUTCOME_SIGNAL)
			return -EINTR;
	}
}

int kernel_wait4(pid_t pid, int options, struct proc_wait_info *result)
{
	if (options & ~(WNOHANG | __WNOTHREAD |
			__WCLONE | __WALL))
		return -EINVAL;
	return wait_child(pid, options, wait4_event_mask(options), result);
}

int kernel_waitid(pid_t pid, int options, struct proc_wait_info *result)
{
	uint32_t events = 0;

	if (options & ~(WNOHANG | WEXITED | WNOWAIT |
			__WNOTHREAD | __WCLONE | __WALL))
		return -EINVAL;
	if (options & WEXITED)
		events |= PROC_WAIT_EXIT;
	return events ? wait_child(pid, options, events, result) : -EINVAL;
}
