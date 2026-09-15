/*
 * kernel/fork.c - clone transaction and proc/task construction
 */

#include <nuvix/cpu.h>
#include <nuvix/errno.h>
#include <nuvix/fdtable.h>
#include <nuvix/string.h>
#include <nuvix/slab.h>
#include <nuvix/fork.h>
#include <nuvix/fs_struct.h>
#include <nuvix/mm.h>
#include <nuvix/proc.h>
#include <nuvix/sched.h>
#include <nuvix/session.h>
#include <nuvix/signal.h>
#include <nuvix/task.h>
#include <nuvix/wait.h>
#include <uapi/sched.h>
#include <uapi/signal.h>

#define CLONE_EXIT_SIGNAL_MASK 0xffULL

#define CLONE_SUPPORTED_FLAGS                                                  \
	(CLONE_EXIT_SIGNAL_MASK | CLONE_VM | CLONE_FS | CLONE_FILES |          \
	 CLONE_SIGHAND | CLONE_VFORK | CLONE_PARENT | CLONE_THREAD |           \
	 CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID |                  \
	 CLONE_DETACHED | CLONE_UNTRACED |              \
	 CLONE_CHILD_SETTID)

#define CLONE_UNSUPPORTED_FLAGS                                                \
	(CLONE_NEWTIME | CLONE_PIDFD | CLONE_PTRACE | CLONE_NEWNS |            \
	 CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER |       \
	 CLONE_NEWPID | CLONE_NEWNET | CLONE_IO | CLONE_CLEAR_SIGHAND |        \
	 CLONE_INTO_CGROUP | CLONE_CHILD_CLEARTID)

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
	    (!(flags & CLONE_VM) || !(flags & CLONE_SIGHAND)))
		return -EINVAL;
	(void)child_stack;
	if (!clone_wants_thread(flags) && exit_signal != 0 &&
	    !sig_valid((int)exit_signal))
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
	struct mm_struct *parent_mm = task->mm;
	struct mm_struct *mm;
	struct mm_struct *oldmm;

	if (flags & CLONE_VM) {
		mm = parent_mm;
		if (parent_mm)
			mm_get(parent_mm);
	} else {
		mm = dup_mm(parent_mm);
		if (parent_mm && !mm)
			return -ENOMEM;
		/* dup_mm inherits fixed mappings, including the signal trampoline. */
	}
	oldmm = task_replace_mm(child, mm);
	mm_put(oldmm);
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

	*new_proc = !(flags & CLONE_THREAD);
	if (!*new_proc) {
		proc_get(parent);
		child->proc = parent;
		return 0;
	}
	if (flags & CLONE_PARENT)
		parent = proc_parent_get(parent);
	else
		proc_get(parent);
	proc = proc_alloc(parent, child->tid);
	proc_put(parent);
	if (!proc)
		return -ENOMEM;
	child->proc = proc;
	proc_inherit_user_process(proc, current_task()->proc);
	return 0;
}

static int clone_copy_resources(struct task_struct *child, unsigned long flags,
				bool new_proc)
{
	struct task_struct *parent = current_task();
	int ret;

	/* Group membership does not imply sharing either resource. The child
	 * owns these references even when it joins the parent's thread group. */
	if (flags & CLONE_FILES) {
		child->files = parent->files;
		files_get(child->files);
	} else {
		child->files = files_dup(parent->files);
		if (!child->files)
			return -ENOMEM;
	}
	if (flags & CLONE_FS) {
		child->fs = parent->fs;
		fs_get(child->fs);
	} else {
		child->fs = fs_dup(parent->fs);
		if (!child->fs)
			return -ENOMEM;
	}

	if (!new_proc) {
		clone_copy_task_signal(child, current_task(),
				       (flags & CLONE_VM) != 0);
		return 0;
	}
	ret = proc_clone_rlimits(child->proc, current_task()->proc);
	if (ret < 0)
		return ret;
	ret = sig_task_clone(child, (flags & CLONE_SIGHAND) != 0, false);
	if (ret < 0)
		return ret;
	return 0;
}

static int clone_copy_cred(struct task_struct *child)
{
	struct cred *source __cleanup_with(cred_ref) =
		task_cred_get(current_task());
	struct cred *cred;

	if (!source)
		return -EINVAL;
	cred = cred_dup(source);
	if (!cred)
		return -ENOMEM;
	cred_put(child->cred);
	child->cred = cred;
	return 0;
}

static void vfork_put(struct vfork_completion *vfork)
{
	if (vfork && refcount_dec_and_test(&vfork->refs))
		kfree(vfork);
}

static void clone_wait_for_vfork(struct vfork_completion *vfork)
{
	const struct wait_deadline deadline = wait_deadline_none();

	for (;;) {
		struct wait_scope scope __wait_scope = {};
		wait_outcome_t outcome;
		irq_flags_t flags;
		int ret;
		bool completed;

		ret = wait_scope_begin(&scope, WAIT_FLAG_KILLABLE, &deadline);
		BUG_ON(ret < 0);
		spin_lock_irqsave(&vfork->lock, &flags);
		completed = vfork->completed;
		if (!completed)
			ret = wait_scope_prepare(&scope,
						 &vfork->channel, true);
		spin_unlock_irqrestore(&vfork->lock, flags);
		if (ret < 0) {
			wait_scope_complete(&scope);
			BUG_ON(ret < 0);
		}
		if (completed) {
			wait_scope_complete(&scope);
			return;
		}
		ret = wait_scope_block(&scope, &outcome);
		wait_scope_complete(&scope);
		BUG_ON(ret < 0);
		if (outcome == WAIT_OUTCOME_SIGNAL)
			return;
		BUG_ON(outcome != WAIT_OUTCOME_EVENT);
	}
}

int kernel_clone_prepare(struct trap_frame *tf, unsigned long flags,
			 uintptr_t child_stack, uintptr_t tls,
			 int *child_tid, struct kernel_clone *clone)
{
	struct task_struct *task = current_task();
	struct task_struct *child;
	bool new_proc = false;
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
	sched_task_inherit_affinity(child, task);
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
	if (flags & CLONE_CHILD_SETTID)
		child->signal.set_child_tid = child_tid;
	if (new_proc) {
		child->proc->wait_state.exit_signal =
			(int)(flags & CLONE_EXIT_SIGNAL_MASK);
		child->proc->wait_state.creator_id = (flags & CLONE_PARENT) ?
			task->proc->wait_state.creator_id : task->identity;
		ret = session_process_clone_prepare(child, task, false);
		if (ret < 0)
			goto fail_proc;
	}
	if (flags & CLONE_VFORK) {
		child->vfork = kzalloc(sizeof(*child->vfork), ALLOC_NOWAIT);
		if (!child->vfork) {
			ret = -ENOMEM;
			goto fail_proc;
		}
		refcount_set(&child->vfork->refs, 1);
		spin_lock_init(&child->vfork->lock, LOCK_RANK_VFORK, LOCK_IRQ_TASK_ONLY);
		wait_channel_init(&child->vfork->channel);
	}
	clone->task = child;
	clone->flags = flags;
	clone->pid = child->tid->nr;
	clone->new_proc = new_proc;
	clone->child_tid = child_tid;
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
	struct task_struct *child = clone->task;
	struct vfork_completion *vfork = child->vfork;
	int ret;

	if (vfork)
		refcount_inc(&vfork->refs);
	ret = task_publish(child);

	if (ret < 0) {
		if (vfork)
			vfork_put(vfork);
		kernel_clone_abort(clone);
		return ret;
	}
	clone->task = NULL;
	if (vfork) {
		clone_wait_for_vfork(vfork);
		vfork_put(vfork);
	}
	return clone->pid;
}

void kernel_clone_abort(struct kernel_clone *clone)
{
	if (!clone || !clone->task)
		return;
	clone_abort_task(clone->task, clone->new_proc);
	clone->task = NULL;
}

void kernel_clone_complete_vfork(struct task_struct *task)
{
	struct vfork_completion *vfork = task->vfork;

	if (!vfork)
		return;
	task->vfork = NULL;
	spin_lock(&vfork->lock);
	vfork->completed = true;
	spin_unlock(&vfork->lock);
	wait_channel_wake_all(&vfork->channel);
	vfork_put(vfork);
}
