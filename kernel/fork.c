/*
 * kernel/fork.c - clone transaction and proc/task construction
 */

#include "kernel/cpu.h"
#include <kernel/errno.h>
#include <kernel/fdtable.h>
#include <kernel/fork.h>
#include <kernel/fs_struct.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/rseq.h>
#include <kernel/sched.h>
#include <kernel/session.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/wait.h>
#include <uapi/sched.h>
#include <uapi/signal.h>

#define CLONE_EXIT_SIGNAL_MASK 0xffULL

#define CLONE_SUPPORTED_FLAGS                                                  \
	(CLONE_EXIT_SIGNAL_MASK | CLONE_VM | CLONE_FS | CLONE_FILES |          \
	 CLONE_SIGHAND | CLONE_VFORK | CLONE_PARENT | CLONE_THREAD |           \
	 CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID |                  \
	 CLONE_CHILD_CLEARTID | CLONE_DETACHED | CLONE_UNTRACED |              \
	 CLONE_CHILD_SETTID)

#define CLONE_UNSUPPORTED_FLAGS                                                \
	(CLONE_NEWTIME | CLONE_PIDFD | CLONE_PTRACE | CLONE_NEWNS |            \
	 CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER |       \
	 CLONE_NEWPID | CLONE_NEWNET | CLONE_IO | CLONE_CLEAR_SIGHAND |        \
	 CLONE_INTO_CGROUP)

static bool clone_wants_thread(unsigned long flags)
{
	return (flags & CLONE_THREAD) != 0;
}

static int validate_clone_flags(unsigned long flags, uintptr_t child_stack)
{
	unsigned long exit_signal = flags & CLONE_EXIT_SIGNAL_MASK;

	if (flags & ~(CLONE_SUPPORTED_FLAGS | CLONE_UNSUPPORTED_FLAGS))
		return -EINVAL;
	if (flags & CLONE_UNSUPPORTED_FLAGS)
		return -EINVAL;
	if ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM))
		return -EINVAL;
	if ((flags & CLONE_THREAD) &&
	    (!(flags & CLONE_VM) || !(flags & CLONE_SIGHAND) || exit_signal))
		return -EINVAL;
	if ((flags & CLONE_VFORK) && (flags & CLONE_THREAD))
		return -EINVAL;
	if ((flags & CLONE_VM) && child_stack == 0)
		return -EINVAL;
	if (!clone_wants_thread(flags) && exit_signal != 0 &&
	    !signal_is_valid((int)exit_signal))
		return -EINVAL;
	if ((flags & CLONE_PARENT) && current_task()->proc &&
	    current_task()->proc->pid && current_task()->proc->pid->nr == 1)
		return -EINVAL;
	return 0;
}

static void clone_copy_task_signal(struct task_struct *child,
				   const struct task_struct *parent,
				   bool disable_altstack)
{
	child->signal.blocked = parent->signal.blocked;
	child->signal.pending = 0;
	child->signal.forced_pending = 0;
	memset(child->signal.pending_info, 0,
	       sizeof(child->signal.pending_info));
	child->signal.restore_mask = 0;
	child->signal.restore_mask_pending = false;
	child->signal.signal_frames = NULL;
	if (disable_altstack)
		child->signal.sas.ss_flags = SS_DISABLE;
	else {
		child->signal.sas = parent->signal.sas;
		child->signal.sas.ss_flags &= ~SS_ONSTACK;
	}
}

static int clone_copy_mm(struct task_struct *child, unsigned long flags)
{
	struct task_struct *task = current_task();
	struct mm_struct *parent_mm = task->proc ? task->proc->mm : NULL;
	struct mm_struct *mm;
	struct mm_struct *oldmm;

	if (clone_wants_thread(flags)) {
		/* Threads share their proc mm, but still need their own arch
		 * state. */
		if (child->proc && child->proc->mm)
			child->arch.pgroot = mm_pgroot(child->proc->mm);
		return 0;
	}
	if (flags & CLONE_VM) {
		mm = parent_mm;
		if (parent_mm)
			mm_get(parent_mm);
	} else {
		mm = dup_mm(parent_mm);
		if (parent_mm && !mm)
			return -ENOMEM;
	}
	oldmm = proc_replace_mm(child->proc, mm);
	mm_put(oldmm);
	if (mm)
		child->arch.pgroot = mm_pgroot(mm);
	return 0;
}

static void clone_abort_task(struct task_struct *child, bool new_proc)
{
	struct proc_struct *proc;

	if (!child)
		return;
	proc = child->proc;
	if (proc) {
		if (new_proc) {
			proc_release_resources(proc);
			(void)proc_unlink_child(proc, NULL, 0);
			proc->lifecycle = PROC_DEAD;
		}
		proc_detach_task(proc, child);
	}
	child->lifecycle = TASK_DEAD;
	task_free(child);
}

static int clone_prepare_proc(struct task_struct *child, unsigned long flags,
			      bool *new_proc)
{
	struct proc_struct *parent = current_task()->proc;
	struct proc_struct *proc;
	int ret;

	*new_proc = false;
	if (clone_wants_thread(flags)) {
		if (!parent)
			return -EINVAL;
		ret = proc_attach_task(parent, child, false);
		if (ret < 0)
			return ret;
		return 0;
	}
	proc = proc_alloc((flags & CLONE_PARENT) && parent ? parent->parent
							   : parent,
			  child->tid);
	if (!proc)
		return -ENOMEM;
	proc_inherit_user_process(proc, parent);
	ret = proc_attach_task(proc, child, true);
	if (ret < 0) {
		proc_put(proc);
		return ret;
	}
	ret = proc_link_child(proc->parent, proc);
	if (ret < 0) {
		proc_detach_task(proc, child);
		proc->lifecycle = PROC_DEAD;
		proc_put(proc);
		return ret;
	}
	proc_put(proc); /* task membership owns the proc reference */
	*new_proc = true;
	return 0;
}

static int clone_copy_resources(struct task_struct *child, unsigned long flags,
				bool new_proc)
{
	int ret;

	if (!new_proc) {
		clone_copy_task_signal(child, current_task(),
				       (flags & CLONE_VM) != 0);
		return 0;
	}
	ret = proc_clone_rlimits(child->proc, current_task()->proc);
	if (ret < 0)
		return ret;
	ret = copy_files(current_task()->proc, child->proc,
			 (flags & CLONE_FILES) != 0);
	if (ret < 0)
		return ret;
	ret = copy_fs(current_task()->proc, child->proc,
		      (flags & CLONE_FS) != 0);
	if (ret < 0)
		return ret;
	ret = signals_clone(child, (flags & CLONE_SIGHAND) != 0, false);
	if (ret < 0)
		return ret;
	return 0;
}

static int clone_copy_cred(struct task_struct *child)
{
	struct cred *cred = cred_dup(current_task()->cred);

	if (!cred)
		return -ENOMEM;
	cred_put(child->cred);
	child->cred = cred;
	return 0;
}

static void clone_wait_for_vfork(struct proc_struct *proc)
{
	const struct wait_deadline deadline = wait_deadline_none();
	struct task_wait *wait = &current_task()->wait;

	for (;;) {
		wait_outcome_t outcome;
		irq_flags_t flags;
		int ret;
		bool completed;

		ret = wait_start(wait, WAIT_FLAG_KILLABLE, &deadline);
		BUG_ON(ret < 0);
		spin_lock_irqsave(&proc->vfork.lock, &flags);
		completed = proc->vfork.completed;
		if (!completed)
			ret = wait_prepare(wait, &proc->vfork.channel, true);
		spin_unlock_irqrestore(&proc->vfork.lock, flags);
		if (ret < 0) {
			wait_finish(wait);
			BUG_ON(ret < 0);
		}
		if (completed) {
			wait_finish(wait);
			return;
		}
		ret = wait_block(wait, &outcome);
		wait_finish(wait);
		BUG_ON(ret < 0);
		if (outcome == WAIT_OUTCOME_SIGNAL)
			return;
		BUG_ON(outcome != WAIT_OUTCOME_EVENT);
	}
}

int kernel_clone_prepare(struct trap_frame *tf, unsigned long flags,
			 uintptr_t child_stack, uintptr_t tls,
			 int *clear_child_tid, struct kernel_clone *clone)
{
	struct task_struct *task = current_task();
	struct task_struct *child;
	bool new_proc;
	int ret;

	if (!clone || !task)
		return -EINVAL;
	memset(clone, 0, sizeof(*clone));
	ret = validate_clone_flags(flags, child_stack);
	if (ret < 0)
		return ret;
	child = task_alloc();
	if (!child)
		return -ENOMEM;
	ret = clone_prepare_proc(child, flags, &new_proc);
	if (ret < 0)
		goto fail;
	ret = clone_copy_cred(child);
	if (ret < 0)
		goto fail_proc;
	ret = clone_copy_mm(child, flags);
	if (ret < 0)
		goto fail_proc;
	task_setup_clone_frame(child, tf, flags, child_stack, tls);
	ret = clone_copy_resources(child, flags, new_proc);
	if (ret < 0)
		goto fail_proc;
	if ((flags & CLONE_CHILD_CLEARTID) && clear_child_tid)
		child->signal.clear_child_tid = clear_child_tid;
	if (flags & CLONE_CHILD_SETTID)
		child->signal.set_child_tid = clear_child_tid;
	rseq_clone(child, task, flags);
	if (new_proc) {
		child->proc->wait_state.exit_signal =
			(int)(flags & CLONE_EXIT_SIGNAL_MASK);
		child->proc->wait_state.creator_tid =
			task->tid ? task->tid->nr : 0;
		if (flags & CLONE_VFORK) {
			child->proc->vfork.active = true;
			child->proc->vfork.completed = false;
		}
		ret = session_process_clone_prepare(child, task, false);
		if (ret < 0)
			goto fail_proc;
	}
	clone->task = child;
	clone->flags = flags;
	clone->pid = child->tid->nr;
	clone->new_proc = new_proc;
	clone->child_tid = clear_child_tid;
	return 0;

fail_proc:
	clone_abort_task(child, new_proc);
	return ret;
fail:
	child->lifecycle = TASK_DEAD;
	task_free(child);
	return ret;
}

pid_t kernel_clone_commit(struct kernel_clone *clone)
{
	struct task_struct *child;
	struct proc_struct *proc;
	bool wait_for_vfork;

	if (!clone || !clone->task)
		return -EINVAL;
	child = clone->task;
	proc = child->proc;
	wait_for_vfork = (clone->flags & CLONE_VFORK) != 0;
	if (wait_for_vfork)
		proc_get(proc);
	task_publish(child);
	sched_enqueue_new(child);
	clone->task = NULL;
	if (wait_for_vfork) {
		clone_wait_for_vfork(proc);
		proc_put(proc);
	}
	return clone->pid;
}

void kernel_clone_abort(struct kernel_clone *clone)
{
	if (!clone || !clone->task)
		return;
	session_process_abort(clone->task);
	clone_abort_task(clone->task, clone->new_proc);
	clone->task = NULL;
}

void kernel_clone_complete_vfork(struct task_struct *task)
{
	struct proc_vfork_state *vfork;
	bool wake = false;

	if (!task || !task->proc)
		return;
	vfork = &task->proc->vfork;
	spin_lock(&vfork->lock);
	if (vfork->active && !vfork->completed) {
		vfork->completed = true;
		vfork->active = false;
		wake = true;
	}
	spin_unlock(&vfork->lock);
	if (wake)
		wait_channel_wake_all(&vfork->channel);
}
