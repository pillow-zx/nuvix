/*
 * kernel/fork.c - fork/clone 进程与线程创建
 */

#include <kernel/errno.h>
#include <kernel/fdtable.h>
#include <kernel/futex.h>
#include <kernel/fork.h>
#include <kernel/fs_struct.h>
#include <kernel/mm.h>
#include <kernel/printk.h>
#include <kernel/rseq.h>
#include <kernel/sched.h>
#include <kernel/session.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <uapi/sched.h>

#include "task_internal.h"

constexpr uint64_t CLONE_EXIT_SIGNAL_MASK = 0xffULL;

constexpr uint64_t KNOWN_CLONE_FLAGS =
	CLONE_EXIT_SIGNAL_MASK | CLONE_VM | CLONE_FS | CLONE_FILES |
	CLONE_SIGHAND | CLONE_PIDFD | CLONE_PTRACE | CLONE_VFORK |
	CLONE_PARENT | CLONE_THREAD | CLONE_NEWNS | CLONE_SYSVSEM |
	CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID |
	CLONE_DETACHED | CLONE_UNTRACED | CLONE_CHILD_SETTID | CLONE_NEWCGROUP |
	CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID |
	CLONE_NEWNET | CLONE_IO | CLONE_CLEAR_SIGHAND | CLONE_INTO_CGROUP;

constexpr uint64_t UNSUPPORTED_CLONE_FLAGS =
	CLONE_NEWTIME | CLONE_PIDFD | CLONE_PTRACE | CLONE_PARENT |
	CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC |
	CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET | CLONE_IO |
	CLONE_CLEAR_SIGHAND | CLONE_INTO_CGROUP;

constexpr uint64_t THREAD_ONLY_CLONE_FLAGS =
	CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | CLONE_SETTLS;

static bool clone_wants_thread(unsigned long flags)
{
	return (flags & CLONE_THREAD) != 0;
}

static int validate_clone_flags(unsigned long flags, uintptr_t child_stack)
{
	unsigned long exit_signal = flags & CLONE_EXIT_SIGNAL_MASK;

	if (flags & ~KNOWN_CLONE_FLAGS)
		return -EINVAL;
	if (flags & UNSUPPORTED_CLONE_FLAGS)
		return -EINVAL;
	if ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM))
		return -EINVAL;
	if ((flags & CLONE_VFORK) && (flags & CLONE_THREAD))
		return -EINVAL;

	if ((flags & CLONE_VM) && child_stack == 0)
		return -EINVAL;
	if ((flags & CLONE_VM) && !(flags & CLONE_SIGHAND) &&
	    !(flags & CLONE_VFORK))
		return -EINVAL;
	if ((flags & CLONE_THREAD) && !(flags & CLONE_VM))
		return -EINVAL;
	if ((flags & CLONE_THREAD) && !(flags & CLONE_SIGHAND))
		return -EINVAL;
	if (!clone_wants_thread(flags) && !task_is_group_leader(current_task()))
		return -EINVAL;
	if (!clone_wants_thread(flags) && (flags & THREAD_ONLY_CLONE_FLAGS))
		return -EINVAL;

	if (!clone_wants_thread(flags) && exit_signal != 0 &&
	    exit_signal != SIGCHLD)
		return -EINVAL;

	return 0;
}

static void child_cleanup(struct task_struct *child)
{
	struct mm_struct *mm;

	if (!child)
		return;

	session_process_abort(child);
	close_files(child);
	exit_fs(child);
	signals_release(child);
	mm = task_mm(child);
	if (mm) {
		mm_put(mm);
		task_set_mm(child, NULL);
	}
	task_free(child);
}

static int clone_setup_mm(struct task_struct *child, unsigned long flags)
{
	struct mm_struct *mm;
	struct mm_struct *parent_mm = task_mm(current_task());

	if (flags & CLONE_VM) {
		task_set_mm(child, parent_mm);
		mm_get(parent_mm);
	} else {
		task_set_mm(child, dup_mm(parent_mm));
		if (!task_mm(child) && parent_mm)
			return -ENOMEM;
	}

	mm = task_mm(child);
	if (mm)
		task_set_satp(child, mm_user_satp(mm));

	return 0;
}

static int clone_copy_resources(struct task_struct *child, unsigned long flags)
{
	bool disable_altstack;
	int ret = copy_files(child, (bool)(flags & CLONE_FILES));
	if (ret < 0)
		return ret;

	ret = copy_fs(child, (bool)(flags & CLONE_FS));
	if (ret < 0)
		return ret;

	task_set_uid(child, task_uid(current_task()));
	task_set_gid(child, task_gid(current_task()));
	disable_altstack = (flags & CLONE_VM) && !(flags & CLONE_VFORK);
	ret = signals_clone(child, (bool)(flags & CLONE_SIGHAND),
			    clone_wants_thread(flags), disable_altstack);
	if (ret < 0)
		return ret;

	return 0;
}

static void clone_setup_task_links(struct task_struct *child,
				   unsigned long flags)
{
	child->lifecycle.exit_signal = (int)(flags & CLONE_EXIT_SIGNAL_MASK);
	if (clone_wants_thread(flags)) {
		struct task_struct *leader = task_group_leader(current_task());

		child->ids.tgid = task_tgid(current_task());
		child->ids.group_leader = leader;
		child->lifecycle.exit_signal = 0;
		child->links.parent = leader;
		return;
	}

	child->ids.tgid = child->ids.pid;
	child->ids.group_leader = child;
}

static void clone_link_task(struct task_struct *child, unsigned long flags)
{
	if (clone_wants_thread(flags))
		list_add_tail(
			&child->links.thread_node,
			&task_group_leader(current_task())->links.thread_group);
	else
		task_link_child(current_task(), child);
}

static void clone_setup_vfork(struct task_struct *child, unsigned long flags)
{
	if (!(flags & CLONE_VFORK))
		return;

	spin_lock_init(&child->vfork.lock);
	wait_channel_init(&child->vfork.completion_wait);
	child->vfork.active = true;
	child->vfork.completed = false;
}

static int clone_vfork_probe(struct wait_session *session, void *arg)
{
	struct task_vfork_context *vfork = arg;
	irq_flags_t flags;
	int ret = 0;

	spin_lock_irqsave(&vfork->lock, &flags);
	if (vfork->completed)
		ret = 1;
	else
		ret = wait_session_watch(session, &vfork->completion_wait);
	spin_unlock_irqrestore(&vfork->lock, flags);
	return ret;
}

static void clone_wait_for_vfork(struct task_struct *child)
{
	const struct wait_deadline deadline = wait_deadline_none();
	struct wait_request source = {
		.kind = WAIT_KIND_GENERIC,
		.check = clone_vfork_probe,
		.arg = &child->vfork,
		.channel_limit = 1,
	};
	wait_outcome_t outcome;
	int ret;

	ret = wait_for_killable(&source, &deadline, &outcome);
	BUG_ON(ret < 0);
	BUG_ON(outcome != WAIT_OUTCOME_EVENT && outcome != WAIT_OUTCOME_SIGNAL);
}

int kernel_clone_prepare(struct trap_frame *tf, unsigned long flags,
			 uintptr_t child_stack, uintptr_t tls,
			 int *clear_child_tid, struct kernel_clone *clone)
{
	struct task_struct *child;
	int ret;

	if (!clone)
		return -EINVAL;
	memset(clone, 0, sizeof(*clone));

	ret = validate_clone_flags(flags, child_stack);
	if (ret < 0)
		return ret;

	child = task_alloc();
	if (!child)
		return -ENOMEM;
	task_inherit_process_role(child, current_task());

	ret = clone_setup_mm(child, flags);
	if (ret < 0) {
		child_cleanup(child);
		return ret;
	}

	task_setup_clone_frame(child, tf, flags, child_stack, tls);

	ret = clone_copy_resources(child, flags);
	if (ret < 0) {
		child_cleanup(child);
		return ret;
	}

	if (flags & CLONE_CHILD_CLEARTID)
		task_set_clear_child_tid(child, clear_child_tid);

	rseq_clone(child, current_task(), flags);
	clone_setup_task_links(child, flags);
	ret = session_process_clone_prepare(child, current_task(),
					    clone_wants_thread(flags));
	if (ret < 0) {
		child_cleanup(child);
		return ret;
	}
	clone_setup_vfork(child, flags);
	clone->task = child;
	clone->flags = flags;
	clone->pid = task_pid(child);
	return 0;
}

pid_t kernel_clone_commit(struct kernel_clone *clone)
{
	struct task_struct *child;
	bool wait_for_vfork;

	if (!clone || !clone->task)
		return -EINVAL;

	child = clone->task;
	wait_for_vfork = (clone->flags & CLONE_VFORK) != 0;
	clone_link_task(child, clone->flags);
	task_publish(child);
	sched_enqueue(child);
	clone->task = NULL;
	if (wait_for_vfork)
		clone_wait_for_vfork(child);
	return clone->pid;
}

void kernel_clone_abort(struct kernel_clone *clone)
{
	if (!clone || !clone->task)
		return;

	child_cleanup(clone->task);
	clone->task = NULL;
}

void kernel_clone_complete_vfork(struct task_struct *task)
{
	struct task_vfork_context *vfork;
	irq_flags_t flags;
	bool wake = false;

	if (!task || !task->vfork.active)
		return;
	vfork = &task->vfork;

	spin_lock_irqsave(&vfork->lock, &flags);
	if (!vfork->completed) {
		vfork->completed = true;
		wake = true;
	}
	spin_unlock_irqrestore(&vfork->lock, flags);

	if (wake)
		wait_channel_wake_all(&vfork->completion_wait);
}
