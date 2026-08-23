/*
 * kernel/exit.c - task exit and proc-owned zombie/reap state
 */

#include <nuvix/errno.h>
#include <nuvix/exit.h>
#include <nuvix/fdtable.h>
#include <nuvix/fork.h>
#include <nuvix/futex.h>
#include <nuvix/mm.h>
#include <nuvix/pgtable.h>
#include <nuvix/printk.h>
#include <nuvix/proc.h>
#include <nuvix/processor.h>
#include <nuvix/resource.h>
#include <nuvix/rseq.h>
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
	uint32_t event_mask = PROC_WAIT_EXIT;

	if (options & WUNTRACED)
		event_mask |= PROC_WAIT_STOP;
	if (options & WCONTINUED)
		event_mask |= PROC_WAIT_CONTINUE;
	return event_mask;
}

static void clear_child_tid(struct task_struct *task)
{
	int *clear_tid = task_clear_child_tid(task);
	int zero = 0;
	struct mm_struct *mm;

	if (!clear_tid)
		return;
	mm = task->proc ? task->proc->mm : NULL;
	if (mm && copy_to_user(clear_tid, &zero, sizeof(zero)) == 0 &&
	    futex_wake_mm(mm, clear_tid, 1) < 0)
		pr_debug("futex: clear_child_tid wake failed\n");
	task_set_clear_child_tid(task, NULL);
}

static void release_proc_mm(struct proc_struct *proc, bool current)
{
	struct mm_struct *mm;

	mm = proc_replace_mm(proc, NULL);
	if (!mm)
		return;
	if (current) {
		activate_pgroot(kernel_pgroot());
		sched_publish_active_mm(NULL);
	}
	mm_put(mm);
}

static void detach_from_scheduler(struct task_struct *task)
{
	if (task && task->on_rq)
		sched_dequeue(task);
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
	struct proc_struct *proc;
	bool last_task;
	bool current;


	if (!task_begin_exit(task))
		return;

	current = task == current_task();
	detach_from_scheduler(task);
	if (current)
		wait_finish(&task->wait);

	restart_clear(task);
	task_set_exit_code(task, status);
	futex_exit_robust_list(task);
	clear_child_tid(task);

	proc = task->proc;
	if (!proc) {
		task_mark_dead(task);
		return;
	}

	proc_get(proc);
	last_task = proc_is_last_task(proc, task);
	account_task_to_proc(proc, task);
	if (last_task) {
		release_proc_mm(proc, current);
		proc_release_resources(proc);
	}
	sig_task_release(task);
	if (last_task)
		kernel_clone_complete_vfork(task);
	/* Session/TTY cleanup runs when the proc dies, even if the exiting
	 * task is not the group leader (leader exited earlier with siblings).
	 */
	if (last_task)
		session_process_exit(task);
	(void)proc_detach_task(proc, task);

	if (last_task) {
		int process_status = proc->group_exit_status_valid
					     ? proc->group_exit_status
					     : status;
		struct proc_struct *parent = proc_parent_get(proc);
		struct sigchld_exit_policy policy =
			parent ? sigchld_exit_policy(parent)
			       : (struct sigchld_exit_policy){0};
		bool sigchld_exit = proc->wait_state.exit_signal == SIGCHLD;
		bool auto_reap = sigchld_exit && policy.auto_reap;
		bool notify_sigchld = sigchld_exit && policy.notify;

		proc_prepare_exit(proc, process_status, task_uid(task),
				  auto_reap, notify_sigchld);
		proc_put(parent);
		/* The temporary proc reference becomes the reaper handoff. */
		task->reap_proc = proc;
	} else {
		proc_put(proc);
	}
	task_mark_dead(task);
}

static void task_reaper_thread(void *arg)
{
	(void)arg;
	for (;;) {
		struct task_struct *task;
		struct wait_deadline deadline;
		int ret;

		while (sched_retired_pop(&task)) {
			BUG_ON(!task_reap_ready(task));
			release_task(task);
		}

		deadline = wait_deadline_at(
			mtime_deadline_after(timer_now(), CLOCKS_PER_TICK));
		ret = wait_sleep_until(&deadline);
		(void)ret;
	}
}

int task_reaper_start(void)
{
	return kernel_thread(task_reaper_thread, NULL) ? 0 : -ENOMEM;
}

__noreturn
static void exit_schedule(void)
{
	sched_exit_current();
	panic("exit: scheduler returned for an exiting task");
	unreachable();
}

static void exit_group_status(struct proc_struct *proc, int status)
{
	struct task_struct *current = current_task();
	struct task_struct **targets;
	size_t count;

	BUG_ON(!current);
	if (!proc)
		return;
	if (!proc_begin_group_exit(proc, status) &&
	    proc->group_exit_status_valid)
		status = proc->group_exit_status;

	/* Every live sibling must receive the exit request; the exit path
	 * cannot fail or truncate, so allocation failure is fatal. */
	targets = kmalloc_array(PID_COUNT, sizeof(*targets), ALLOC_NOWAIT);
	BUG_ON(!targets);

	count = proc_task_snapshot(proc, current, targets, PID_COUNT);
	for (size_t index = 0; index < count; index++) {
		(void)task_request_group_exit(targets[index]);
		task_put(targets[index]);
	}
	kfree(targets);
	finish_task_exit(current, status);
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
	struct proc_orphan_event *orphan_events;
	struct proc_parent_event parent_event;
	struct proc_struct *proc;
	size_t orphan_count;

	if (!task)
		return;
	BUG_ON(task_is_idle(task));
	BUG_ON(!task_reap_ready(task));
	BUG_ON(!list_empty(&task->retired_node));
	proc = task->reap_proc;
	task->reap_proc = NULL;
	task_reap_unpublish(task);
	if (proc) {
		/* The reaper must finish the exit regardless of memory
		 * pressure; a missed orphan event would silently break
		 * job control, so allocation failure is fatal. */
		orphan_events = kmalloc_array(PID_COUNT,
					      sizeof(*orphan_events),
					      ALLOC_NOWAIT);
		BUG_ON(!orphan_events);
		orphan_count =
			proc_publish_exit(proc, orphan_events, PID_COUNT,
					  &parent_event);
		sig_notify_parent(&parent_event);
		proc_parent_event_release(&parent_event);
		for (size_t index = 0; index < orphan_count; index++)
			sig_orphan_pgrp(&orphan_events[index]);
		for (size_t index = 0; index < orphan_count; index++)
			proc_orphan_event_release(&orphan_events[index]);
		kfree(orphan_events);
		/* The pgrp unlink above may have emptied the session: release
		 * the controlling-TTY owner reference if it did. */
		session_process_reaper(proc);
		proc_put(proc);
	}
	task_put(task);
}

static void proc_cputime_total(struct proc_struct *proc,
			       struct task_cputime *time)
{
	struct proc_cputime_snapshot snapshot;

	proc_cputime_snapshot(proc, &snapshot);
	*time = snapshot.self;
	cputime_add(time, &snapshot.children);
}

static void wait4_release_orphans(struct wait4_result *result)
{
	kfree(result->claim.orphan_events);
	result->claim.orphan_events = NULL;
	result->claim.orphan_capacity = 0;
}

int kernel_wait4(pid_t pid, int options, struct wait4_result *result)
{
	const struct wait_deadline deadline = wait_deadline_none();
	struct proc_wait_selector selector;
	struct task_wait *wait = &current_task()->wait;
	wait_outcome_t outcome;
	struct proc_struct *parent = current_task()->proc;
	enum proc_wait_result state;
	int ret;

	if (!result || !parent)
		return -EINVAL;
	if (options & ~(WNOHANG | WUNTRACED | WCONTINUED | __WNOTHREAD |
			__WCLONE | __WALL))
		return -EINVAL;
	memset(result, 0, sizeof(*result));
	/* The orphan-event snapshot for the commit may need up to PID_COUNT
	 * entries; keep it off the kernel stack.  The wait4 flow below owns
	 * it until kernel_wait4_finish()/kernel_wait4_abort(). */
	result->claim.orphan_events =
		kmalloc_array(PID_COUNT, sizeof(*result->claim.orphan_events),
			      ALLOC_NOWAIT);
	if (!result->claim.orphan_events)
		return -ENOMEM;
	result->claim.orphan_capacity = PID_COUNT;
	selector = (struct proc_wait_selector){
		.pid = pid,
		.pgid = parent->pgrp && parent->pgrp->pgid
				? parent->pgrp->pgid->nr
				: 0,
		.creator_tid =
			current_task()->tid ? current_task()->tid->nr : 0,
		.child_class = (options & __WALL)     ? PROC_WAIT_ALL_CHILDREN
			       : (options & __WCLONE) ? PROC_WAIT_CLONE
						      : PROC_WAIT_SIGCHLD,
		.creator_only = (options & __WNOTHREAD) != 0,
	};
	for (;;) {
		state = proc_wait_claim(parent, &selector,
					wait4_event_mask(options),
					&result->claim);
		if (state == PROC_WAIT_NO_CHILD) {
			wait4_release_orphans(result);
			return -ECHILD;
		}
		if (state == PROC_WAIT_EVENT) {
			struct proc_struct *child = result->claim.child;

			result->pid = child->pid->nr;
			result->status = result->claim.status;
			proc_cputime_total(child, &result->cputime);
			return 0;
		}
		if (options & WNOHANG) {
			wait4_release_orphans(result);
			return 0;
		}

		ret = wait_start(wait, WAIT_FLAG_INTERRUPTIBLE, &deadline);
		if (ret < 0) {
			wait4_release_orphans(result);
			return ret;
		}
		ret = proc_wait_watch(parent, &selector,
				      wait4_event_mask(options), wait);
		if (ret < 0) {
			wait_finish(wait);
			wait4_release_orphans(result);
			return ret;
		}
		if (ret > 0) {
			wait_finish(wait);
			continue;
		}
		ret = wait_block(wait, &outcome);
		wait_finish(wait);
		if (ret < 0) {
			wait4_release_orphans(result);
			return ret;
		}
		if (outcome == WAIT_OUTCOME_SIGNAL) {
			wait4_release_orphans(result);
			return -EINTR;
		}
		BUG_ON(outcome != WAIT_OUTCOME_EVENT);
	}
}

void kernel_wait4_finish(struct wait4_result *result)
{
	struct proc_wait_claim claim;
	uint32_t event;
	bool committed;

	if (!result)
		return;
	if (!result->claim.child) {
		wait4_release_orphans(result);
		return;
	}
	claim = result->claim;
	event = claim.event;
	committed = proc_wait_commit(&claim);
	if (!committed)
		return;
	for (size_t index = 0; index < claim.orphan_count; index++)
		sig_orphan_pgrp(&claim.orphan_events[index]);
	for (size_t index = 0; index < claim.orphan_count; index++)
		proc_orphan_event_release(&claim.orphan_events[index]);
	if (event == PROC_WAIT_EXIT && current_task()->proc)
		proc_account_child_cputime(current_task()->proc,
					   &result->cputime);
	wait4_release_orphans(result);
	memset(result, 0, sizeof(*result));
}

void kernel_wait4_abort(struct wait4_result *result)
{
	if (!result || !result->claim.child)
		return;
	proc_wait_abort(&result->claim);
	wait4_release_orphans(result);
	memset(result, 0, sizeof(*result));
}
