/*
 * kernel/signal.c - 信号机制
 */

#include <nuvix/errno.h>
#include <nuvix/exit.h>
#include <nuvix/buddy.h>
#include <nuvix/compiler.h>
#include <nuvix/fs.h>
#include <nuvix/init.h>
#include <nuvix/mm.h>
#include <nuvix/pid.h>
#include <nuvix/printk.h>
#include <nuvix/proc.h>
#include <nuvix/sched.h>
#include <nuvix/signal.h>
#include <nuvix/slab.h>
#include <nuvix/task.h>
#include <nuvix/syscall.h>
#include <nuvix/time.h>
#include <nuvix/wait.h>
#include <uapi/mman.h>
#include <uapi/syscall.h>
#include <nuvix/processor.h>
#include <nuvix/page.h>
#include <nuvix/trap.h>

#define SIGNAL_STANDARD_MASK   ((1UL << (SIGRTMIN - 1)) - 1)
enum signal_default_action {
	SIGNAL_DEFAULT_TERMINATE,
	SIGNAL_DEFAULT_IGNORE,
};

static bool task_wait_accepts_signal(struct task_struct *task, int sig);
static void signal_recalc_facts_locked(struct task_struct *task,
				       struct signal_struct *signal);
static uint64_t signal_mask(int sig);
static uint64_t unblockable_mask(void);
static void signal_reset_altstack(struct task_struct *task);
static void signal_block_mask(struct task_struct *task, uint64_t mask);
static void signal_unblock_mask(struct task_struct *task, uint64_t mask);
static void signal_clear_pending(struct task_struct *task, uint64_t mask);
static int send_group_signal_info(int sig, const siginfo_t *info,
				  struct task_struct *leader);

static enum signal_default_action signal_default_action(int sig)
{
	switch (sig) {
	case SIGCHLD:
	case SIGURG:
	case SIGWINCH:
		return SIGNAL_DEFAULT_IGNORE;
	default:
		return SIGNAL_DEFAULT_TERMINATE;
	}
}

static bool signal_has_fault_info(int sig)
{
	return sig == SIGILL || sig == SIGFPE || sig == SIGSEGV ||
	       sig == SIGBUS || sig == SIGTRAP;
}

static void signal_info_store(struct ksiginfo *destination, int sig,
			      const siginfo_t *source)
{
	memset(destination, 0, sizeof(*destination));
	destination->code = source->si_code;
	if (sig == SIGCHLD) {
		destination->child.pid = source->si_pid;
		destination->child.uid = source->si_uid;
		destination->child.status = source->si_status;
		destination->child.utime = source->si_utime;
		destination->child.stime = source->si_stime;
	} else if (signal_has_fault_info(sig) && source->si_code > 0) {
		destination->fault.addr = (uintptr_t)source->si_addr;
		destination->fault.trapno = source->si_trapno;
	} else {
		destination->sender.pid = source->si_pid;
		destination->sender.uid = source->si_uid;
	}
}

static void signal_info_load(siginfo_t *destination, int sig,
			     const struct ksiginfo *source)
{
	memset(destination, 0, sizeof(*destination));
	destination->si_signo = sig;
	destination->si_code = source->code;
	if (sig == SIGCHLD) {
		destination->si_pid = source->child.pid;
		destination->si_uid = source->child.uid;
		destination->si_status = source->child.status;
		destination->si_utime = source->child.utime;
		destination->si_stime = source->child.stime;
	} else if (signal_has_fault_info(sig) && source->code > 0) {
		destination->si_addr = (void *)source->fault.addr;
		destination->si_trapno = source->fault.trapno;
	} else {
		destination->si_pid = source->sender.pid;
		destination->si_uid = source->sender.uid;
	}
}

static void signal_recalc_task_callback(struct task_struct *task, void *arg)
{
	signal_recalc_facts_locked(task, arg);
}

/* Caller holds the owning process signal lock. */
static __sighandler_t signal_handler_for_task_locked(struct task_struct *task,
						     int sig)
{
	if (!task || !task->proc)
		return SIG_DFL;
	return task->proc->signal.actions[sig].sa_handler;
}

static __sighandler_t signal_handler_for_task(struct task_struct *task, int sig)
{
	struct signal_struct *signal =
		task && task->proc ? &task->proc->signal : NULL;
	__sighandler_t handler = SIG_DFL;
	irq_flags_t flags;

	if (!signal)
		return handler;
	spin_lock_irqsave(&signal->siglock, flags);
	handler = signal_handler_for_task_locked(task, sig);
	spin_unlock_irqrestore(&signal->siglock, flags);
	return handler;
}

static struct sigaction signal_sigchld_action(const struct proc_struct *proc)
{
	struct sigaction action = {0};
	struct signal_struct *signal;
	irq_flags_t flags;

	if (!proc)
		return action;
	signal = &((struct proc_struct *)proc)->signal;
	spin_lock_irqsave(&signal->siglock, flags);
	action = signal->actions[SIGCHLD];
	spin_unlock_irqrestore(&signal->siglock, flags);
	return action;
}

static bool signal_init_default_ignored_locked(struct task_struct *task,
					       int sig)
{
	enum signal_default_action action;

	if (!init_process_is_task(task) ||
	    signal_handler_for_task_locked(task, sig) != SIG_DFL)
		return false;

	action = signal_default_action(sig);
	return action == SIGNAL_DEFAULT_TERMINATE;
}

static bool signal_init_default_ignored(struct task_struct *task, int sig)
{
	enum signal_default_action action;

	if (!init_process_is_task(task) ||
	    signal_handler_for_task(task, sig) != SIG_DFL)
		return false;

	action = signal_default_action(sig);
	return action == SIGNAL_DEFAULT_TERMINATE;
}

/* Raise-time drop predicate: an unblocked notification whose disposition at
 * raise time is ignored (explicit SIG_IGN, or a SIG_DFL default-ignore such
 * as SIGCHLD/SIGURG/SIGWINCH, or suppressed for init) is dropped immediately
 * — never queued, never woken.  Blocked signals are never dropped, and neither
 * are signals this task is synchronously waiting for (sigtimedwait's accepted
 * set): both must stay queued so a later unblock or the wait can consume them
 * even under a default-ignore disposition. Forced/exception signals are never
 * dropped. Installing SIG_IGN later discards existing
 * non-forced pending instances.  Callers hold the thread-group siglock. */
static bool signal_would_drop_at_raise_locked(struct task_struct *task, int sig,
					      bool forced)
{
	__sighandler_t handler;

	if (forced)
		return false;
	handler = signal_handler_for_task_locked(task, sig);
	if (handler == SIG_IGN || handler == SIG_DFL) {
		uint64_t mask = signal_mask(sig);

		if (task->signal.blocked & mask)
			return false;
		if (task_wait_accepts_signal(task, sig))
			return false;
	}
	if (handler == SIG_IGN)
		return true;
	if (handler != SIG_DFL)
		return false;
	if (signal_init_default_ignored_locked(task, sig))
		return true;
	return signal_default_action(sig) == SIGNAL_DEFAULT_IGNORE;
}

static void signal_actions_reset_for_exec_locked(struct proc_struct *proc)
{
	if (!proc)
		return;
	for (int sig = 1; sig < SIGRTMIN; sig++) {
		struct sigaction *action = &proc->signal.actions[sig];

		if (action->sa_handler != SIG_IGN)
			memset(action, 0, sizeof(*action));
	}
}

static bool task_wait_accepts_signal(struct task_struct *task, int sig)
{
	if (!task || !sig_valid(sig))
		return false;
	return sched_wait_accepts_signal(task, signal_mask(sig));
}

/* Start a sigtimedwait generation and remove its accepted signals from the
 * blocked mask as one signal-state transaction.  Signal senders take the same
 * siglock, so a raise cannot observe an active wait with the old mask or the
 * new mask half-published. */
static int signal_wait_start_set(struct task_struct *task,
				 struct wait_scope *scope,
				 const struct wait_deadline *deadline,
				 uint64_t signal_set)
{
	struct signal_struct *signal;
	irq_flags_t flags;
	int ret;

	if (!task || !scope)
		return -EINVAL;
	signal = task->proc ? &task->proc->signal : NULL;
	if (!signal) {
		ret = wait_scope_begin_signal_set(
			scope, WAIT_FLAG_INTERRUPTIBLE, deadline, signal_set);
		if (ret == 0)
			task->signal.blocked &= ~signal_set;
		return ret;
	}

	spin_lock_irqsave(&signal->siglock, flags);
	ret = wait_scope_begin_signal_set_locked(scope, WAIT_FLAG_INTERRUPTIBLE,
						 deadline, signal_set);
	if (ret == 0) {
		task->signal.blocked &= ~signal_set;
		signal_recalc_facts_locked(task, signal);
	}
	spin_unlock_irqrestore(&signal->siglock, flags);
	return ret;
}

static void signal_recalc_facts_locked(struct task_struct *task,
				       struct signal_struct *signal)
{
	if (!task || !signal)
		return;

	/* Ordinary interruption follows Linux's deliverability rule: blocked
	 * pending signals remain observable through sigpending(), but do not
	 * wake or interrupt an ordinary wait. */
	uint64_t pending = task->signal.pending | signal->shared_pending;
	uint64_t deliverable =
		pending & ~(task->signal.blocked & ~unblockable_mask());

	atomic_set(&task->signal.has_pending_signal, deliverable != 0);
	/* PID 1's historical default-fatal protection is retained, while forced
	 * SIGKILL remains fatal. */
	atomic_set(&task->signal.has_fatal_pending,
		   (pending & signal_mask(SIGKILL)) != 0 &&
			   ((task->signal.forced_pending &
			     signal_mask(SIGKILL)) != 0 ||
			    !init_process_is_task(task)));
}

static void signal_recalc_targets_locked(struct signal_struct *signal,
					 struct proc_struct *proc,
					 struct task_struct **targets,
					 size_t count)
{
	if (!signal)
		return;
	if (!targets || count == 0) {
		proc_for_each_task(proc, signal_recalc_task_callback, signal);
		return;
	}
	for (size_t index = 0; index < count; index++)
		signal_recalc_facts_locked(targets[index], signal);
}

static void signal_clear_pending_locked(struct task_struct *task, uint64_t mask)
{
	if (!task)
		return;

	for (int sig = 1; sig < SIGRTMIN; sig++) {
		if (mask & signal_mask(sig))
			memset(&task->signal.pending_info[sig], 0,
			       sizeof(task->signal.pending_info[sig]));
	}
	task->signal.pending &= ~mask;
	task->signal.forced_pending &= ~mask;
}

struct signal_discard_context {
	struct signal_struct *signal;
	uint64_t mask;
	int sig;
};

static void signal_discard_task_callback(struct task_struct *task, void *arg)
{
	struct signal_discard_context *context = arg;

	if (!(task->signal.forced_pending & context->mask)) {
		if (task->signal.pending & context->mask) {
			memset(&task->signal.pending_info[context->sig], 0,
			       sizeof(task->signal.pending_info[context->sig]));
			task->signal.pending &= ~context->mask;
		}
	}
	signal_recalc_facts_locked(task, context->signal);
}

static void signal_discard_ignored_pending_locked(struct proc_struct *proc,
						  int sig)
{
	struct signal_struct *signal = proc ? &proc->signal : NULL;
	uint64_t mask = signal_mask(sig);
	struct signal_discard_context context = {
		.signal = signal,
		.mask = mask,
		.sig = sig,
	};

	if (!signal)
		return;
	if (signal->shared_pending & mask) {
		memset(&signal->shared_pending_info[sig], 0,
		       sizeof(signal->shared_pending_info[sig]));
		signal->shared_pending &= ~mask;
	}
	proc_for_each_task(proc, signal_discard_task_callback, &context);
}

static uint64_t signal_take_restore_mask(struct task_struct *task)
{
	uint64_t mask = task->signal.restore_mask;

	task->signal.restore_mask = 0;
	task->signal.restore_mask_pending = false;
	return mask;
}

static void wake_signal_target(struct task_struct *task, int sig)
{
	uint64_t blocked;
	uint64_t pending;
	uint64_t forced;
	uint64_t mask;
	struct signal_struct *signal;
	irq_flags_t flags;
	bool fatal;
	bool deliverable;
	bool accepted;

	if (!task)
		return;
	mask = signal_mask(sig);
	/* Snapshot the task-directed signal state under the thread-group
	 * siglock; run state is deliberately not read here, the wake/resume
	 * calls re-validate it under their own locks and no-op for non-blocked
	 * targets.  The wait policy is read under the wait lock: a task in a
	 * TASK_WAIT_UNINTERRUPTIBLE wait (kernel mutex, exec serialization)
	 * must not be woken by a signal — its wait's consumer treats only
	 * WAIT_OUTCOME_EVENT as valid, so a signal wake would trip a BUG_ON.
	 * wait_wake_signal re-validates the policy under the wait lock. */
	signal = task->proc ? &task->proc->signal : NULL;
	if (signal) {
		spin_lock_irqsave(&signal->siglock, flags);
		blocked = task->signal.blocked;
		pending = task->signal.pending;
		forced = task->signal.forced_pending;
		spin_unlock_irqrestore(&signal->siglock, flags);
	} else {
		blocked = task->signal.blocked;
		pending = task->signal.pending;
		forced = task->signal.forced_pending;
	}
	accepted = sched_wait_accepts_signal(task, mask);

	fatal = sig == SIGKILL ||
		((pending | forced) & signal_mask(SIGKILL)) != 0;
	deliverable =
		fatal || accepted || !sig_catchable(sig) || !(blocked & mask);
	if (deliverable)
		(void)wait_wake_signal(task, fatal);
	if (sig == SIGKILL)
		(void)sched_resume(task);
}

static void signal_kill_member(struct task_struct *task, void *unused)
{
	(void)unused;
	(void)task_request_group_exit(task);
}

static int signal_kill_group(struct task_struct *task, bool force)
{
	if (!task || !task->proc)
		return -ESRCH;
	if (!force && signal_init_default_ignored(task, SIGKILL))
		return 0;
	(void)proc_begin_group_exit(task->proc, SIGKILL);
	proc_notify_tasks(task->proc, signal_kill_member, NULL);
	return 0;
}

static int send_signal_info_internal(int sig, const siginfo_t *info,
				     struct task_struct *task, bool force)
{
	uint64_t mask;
	struct signal_struct *signal;
	irq_flags_t flags;

	if (!sig_valid(sig))
		return -EINVAL;
	if (sig == SIGKILL)
		return signal_kill_group(task, force);
	if (!info)
		return -EINVAL;
	if (!task || task_is_exiting(task))
		return -ESRCH;

	mask = signal_mask(sig);
	signal = task->proc ? &task->proc->signal : NULL;
	if (signal)
		spin_lock_irqsave(&signal->siglock, flags);
	/* Dropped at raise: an unblocked, ignored (SIG_IGN / default-ignore /
	 * init-suppressed) notification is neither queued nor woken.  Blocked
	 * signals are never dropped. Exception/forced signals are never dropped. */
	if (!(task->signal.blocked & mask) &&
	    signal_would_drop_at_raise_locked(task, sig, force)) {
		if (signal)
			spin_unlock_irqrestore(&signal->siglock, flags);
		return 0;
	}
	if (signal)
		spin_lock(&task->lock);
	else
		spin_lock_irqsave(&task->lock, flags);
	/* This is the authoritative admission check: exit and task-directed
	 * queuing hold the same signal -> wait lock pair while changing state.
	 */
	if (task_is_exiting(task)) {
		if (signal) {
			spin_unlock(&task->lock);
			spin_unlock_irqrestore(&signal->siglock, flags);
		} else {
			spin_unlock_irqrestore(&task->lock, flags);
		}
		return -ESRCH;
	}
	if (!(task->signal.pending & mask)) {
		signal_info_store(&task->signal.pending_info[sig], sig, info);
		task->signal.pending |= mask;
	}
	if (force)
		task->signal.forced_pending |= mask;
	if (signal) {
		signal_recalc_facts_locked(task, signal);
		spin_unlock(&task->lock);
		spin_unlock_irqrestore(&signal->siglock, flags);
	} else
		spin_unlock_irqrestore(&task->lock, flags);
	wake_signal_target(task, sig);

	return 0;
}

struct signal_group_drop_context {
	int sig;
	uint64_t mask;
	bool retain;
};

static void signal_group_drop_task_callback(struct task_struct *task, void *arg)
{
	struct signal_group_drop_context *context = arg;

	if (task_wait_accepts_signal(task, context->sig) ||
	    (task->signal.blocked & context->mask))
		context->retain = true;
}

static bool signal_group_would_drop_at_raise_locked(struct task_struct *leader,
						    int sig)
{
	struct signal_group_drop_context context = {
		.sig = sig,
		.mask = signal_mask(sig),
	};
	__sighandler_t handler;

	if (!leader)
		return false;
	handler = signal_handler_for_task_locked(leader, sig);
	if (handler != SIG_IGN && handler != SIG_DFL)
		return false;
	proc_for_each_task(leader->proc, signal_group_drop_task_callback, &context);
	if (context.retain)
		return false;
	return handler == SIG_IGN ||
	       signal_init_default_ignored_locked(leader, sig) ||
	       signal_default_action(sig) == SIGNAL_DEFAULT_IGNORE;
}

static bool signal_may_send_to_task(struct task_struct *target, int sig)
{
	struct task_struct *sender = current_task();
	struct cred *sender_cred __cleanup_with(cred_ref) = NULL;
	struct cred *target_cred __cleanup_with(cred_ref) = NULL;
	bool allowed;

	if (!sender || !target)
		return false;
	sender_cred = task_cred_get(sender);
	target_cred = task_cred_get(target);
	if (!sender_cred || !target_cred)
		return false;
	allowed = sender_cred->euid == 0 ||
		  sender_cred->euid == target_cred->ruid ||
		  sender_cred->euid == target_cred->suid ||
		  sender_cred->ruid == target_cred->ruid ||
		  sender_cred->ruid == target_cred->suid;
	(void)sig;
	return allowed;
}

static uint64_t current_shared_pending(void)
{
	struct task_struct *task = current_task();
	struct signal_struct *signal = task->proc ? &task->proc->signal : NULL;
	uint64_t pending = 0;
	irq_flags_t flags;

	if (!signal)
		return 0;

	spin_lock_irqsave(&signal->siglock, flags);
	pending = signal->shared_pending;
	spin_unlock_irqrestore(&signal->siglock, flags);
	return pending;
}

static int setup_signal_frame(struct trap_frame *tf, int sig,
			      const siginfo_t *info,
			      const struct sigaction *action)
{
	struct task_struct *task = current_task();
	uint64_t mask = task->signal.restore_mask_pending
		? signal_take_restore_mask(task) : sig_blocked_mask(task);
	int ret = arch_signal_setup(tf, sig, info, action, mask);

	if (ret < 0)
		return ret;
	signal_block_mask(task, signal_mask(sig));
	signal_block_mask(task, action->sa_mask);
	return 0;
}

static int take_pending_from_set(uint64_t set, siginfo_t *info)
{
	struct task_struct *task = current_task();
	struct signal_struct *signal = task->proc ? &task->proc->signal : NULL;

	irq_flags_t flags;
	int sig;

	if (!signal) {
		uint64_t pending = task->signal.pending & set;

		for (sig = 1; sig < SIGRTMIN; sig++) {
			uint64_t mask = signal_mask(sig);

			if (!(pending & mask))
				continue;
			signal_info_load(info, sig, &task->signal.pending_info[sig]);
			signal_clear_pending_locked(task, mask);
			return sig;
		}
		return 0;
	}

	/* Per-task and shared pending are drained under one siglock
	 * acquisition, so no group member can double-consume a shared signal.
	 */
	spin_lock_irqsave(&signal->siglock, flags);
	for (sig = 1; sig < SIGRTMIN; sig++) {
		uint64_t mask = signal_mask(sig);

		if (!(task->signal.pending & set & mask))
			continue;
		signal_info_load(info, sig, &task->signal.pending_info[sig]);
		signal_clear_pending_locked(task, mask);
		signal_recalc_facts_locked(task, signal);
		spin_unlock_irqrestore(&signal->siglock, flags);
		return sig;
	}
	for (sig = 1; sig < SIGRTMIN; sig++) {
		uint64_t mask = signal_mask(sig);

		if (!(signal->shared_pending & set & mask))
			continue;
		signal_info_load(info, sig, &signal->shared_pending_info[sig]);
		signal->shared_pending &= ~mask;
		memset(&signal->shared_pending_info[sig], 0,
		       sizeof(signal->shared_pending_info[sig]));
		signal_recalc_targets_locked(signal, task->proc, NULL, 0);
		signal_recalc_facts_locked(task, signal);
		spin_unlock_irqrestore(&signal->siglock, flags);
		return sig;
	}
	spin_unlock_irqrestore(&signal->siglock, flags);
	return 0;
}

static int next_signal(bool *shared)
{
	struct task_struct *task = current_task();
	uint64_t shared_pending = current_shared_pending();
	uint64_t pending = task->signal.pending | shared_pending;
	uint64_t deliverable;

	*shared = false;
	deliverable = pending & SIGNAL_STANDARD_MASK &
		      ~(sig_blocked_mask(task) & ~unblockable_mask());
	if (!deliverable)
		return 0;

	for (int sig = 1; sig < SIGRTMIN; sig++) {
		if (deliverable & signal_mask(sig)) {
			*shared = (task->signal.pending & signal_mask(sig)) ==
					  0 &&
				  (shared_pending & signal_mask(sig)) != 0;
			return sig;
		}
	}

	return 0;
}

static struct sigaction get_signal_action(int sig)
{
	struct task_struct *task = current_task();
	struct signal_struct *signal =
		task->proc ? &task->proc->signal : NULL;
	struct sigaction action = {0};
	irq_flags_t flags;

	if (!signal)
		return action;
	spin_lock_irqsave(&signal->siglock, flags);
	action = signal->actions[sig];
	spin_unlock_irqrestore(&signal->siglock, flags);
	return action;
}

static bool signal_restartable(size_t nr)
{
	switch (nr) {
	case SYS_read:
	case SYS_write:
	case SYS_wait4:
		return true;
	default:
		return false;
	}
}

bool sig_valid(int sig)
{
	return sig > 0 && sig < SIGRTMIN &&
	       sig != SIGCONT && sig != SIGSTOP && sig != SIGTSTP &&
	       sig != SIGTTIN && sig != SIGTTOU;
}

static uint64_t signal_mask(int sig)
{
	return 1UL << (sig - 1);
}

bool sig_catchable(int sig)
{
	return sig != SIGKILL;
}

static uint64_t unblockable_mask(void)
{
	return signal_mask(SIGKILL);
}

struct sigchld_exit_policy sigchld_exit_policy(const struct proc_struct *proc)
{
	struct sigaction action = signal_sigchld_action(proc);
	struct sigchld_exit_policy policy;

	policy.auto_reap = false;
	policy.notify = action.sa_handler != SIG_IGN;
	return policy;
}

static void signal_reset_altstack(struct task_struct *task)
{
	if (!task)
		return;
	task->signal.sas.ss_sp = NULL;
	task->signal.sas.ss_flags = SS_DISABLE;
	task->signal.sas.ss_size = 0;
}

int sig_proc_init(struct proc_struct *proc)
{
	if (!proc)
		return -EINVAL;
	memset(proc->signal.actions, 0, sizeof(proc->signal.actions));
	proc->signal.shared_pending = 0;
	memset(proc->signal.shared_pending_info, 0,
	       sizeof(proc->signal.shared_pending_info));
	return 0;
}

int sig_task_init(struct task_struct *task)
{
	if (!task || !task->proc)
		return -EINVAL;

	sig_set_mask(task, 0);
	signal_clear_pending(task, ~0UL);
	task->signal.forced_pending = 0;
	atomic_set(&task->signal.has_pending_signal, 0);
	atomic_set(&task->signal.has_fatal_pending, 0);
	task->signal.restore_mask = 0;
	task->signal.restore_mask_pending = false;
	signal_reset_altstack(task);
	return 0;
}

bool sig_task_begin_exit(struct task_struct *task)
{
	struct signal_struct *signal;
	irq_flags_t flags;
	bool begun = false;

	if (!task || task_is_idle(task))
		return false;
	signal = task->proc ? &task->proc->signal : NULL;
	if (!signal)
		return false;
	spin_lock_irqsave(&signal->siglock, flags);
	spin_lock(&task->lock);
	if (task->lifecycle == TASK_LIVE) {
		task->lifecycle = TASK_EXITING;
		begun = true;
	}
	spin_unlock(&task->lock);
	spin_unlock_irqrestore(&signal->siglock, flags);
	return begun;
}

void sig_proc_release(struct proc_struct *proc)
{
	(void)proc;
}

void sig_task_release(struct task_struct *task)
{
	if (!task)
		return;

	sig_set_mask(task, 0);
	signal_clear_pending(task, ~0UL);
	task->signal.forced_pending = 0;
	task->signal.restore_mask = 0;
	task->signal.restore_mask_pending = false;
	signal_reset_altstack(task);
}

int sig_task_clone(struct task_struct *child, bool disable_altstack)
{
	struct task_struct *parent = current_task();
	struct signal_struct *source;
	irq_flags_t flags;

	if (!child || !child->proc || !parent || !parent->proc)
		return -EINVAL;
	source = &parent->proc->signal;
	spin_lock_irqsave(&source->siglock, flags);
	memcpy(child->proc->signal.actions, source->actions,
	       sizeof(source->actions));
	spin_unlock_irqrestore(&source->siglock, flags);
	child->proc->resources_initialized = true;

	sig_task_release(child);
	sig_set_mask(child, sig_blocked_mask(parent));
	signal_clear_pending(child, ~0UL);
	child->signal.forced_pending = 0;
	child->signal.restore_mask = 0;
	child->signal.restore_mask_pending = false;
	if (!disable_altstack) {
		struct stack_t *child_sas = &child->signal.sas;

		*child_sas = parent->signal.sas;
		child_sas->ss_flags &= ~SS_ONSTACK;
	} else {
		signal_reset_altstack(child);
	}
	return 0;
}

void sig_exec_commit(struct task_struct *task)
{
	struct signal_struct *signal;
	irq_flags_t flags;

	BUG_ON(!task->proc);
	signal = &task->proc->signal;
	spin_lock_irqsave(&signal->siglock, flags);
	signal_actions_reset_for_exec_locked(task->proc);
	spin_unlock_irqrestore(&signal->siglock, flags);
	signal_reset_altstack(task);
}

uint64_t sig_pending(const struct task_struct *task)
{
	uint64_t pending;
	struct signal_struct *signal;

	if (!task)
		return 0;

	signal = task->proc ? &task->proc->signal : NULL;
	if (!signal)
		return task->signal.pending & task->signal.blocked;
	pending = task->signal.pending;
	if (signal) {
		irq_flags_t flags;

		spin_lock_irqsave(&signal->siglock, flags);
		pending = (task->signal.pending | signal->shared_pending) &
			  task->signal.blocked;
		spin_unlock_irqrestore(&signal->siglock, flags);
	}
	return pending;
}

bool sig_fatal_pending(struct task_struct *task)
{
	if (!task)
		return false;
	if (compiler_atomic_load_n(&task->exit_request, COMPILER_ATOMIC_ACQUIRE) == TASK_EXIT_REQUEST_EXEC ||
	    (task->proc && compiler_atomic_load_n(&task->proc->group_exit_requested, COMPILER_ATOMIC_ACQUIRE)))
		return true;
	if (task->proc)
		return atomic_read(&task->signal.has_fatal_pending) != 0;
	return (task->signal.pending & signal_mask(SIGKILL)) != 0;
}

uint64_t sig_blocked_mask(struct task_struct *task)
{
	struct signal_struct *signal;
	irq_flags_t flags;
	uint64_t blocked;

	if (!task)
		return 0;
	signal = task->proc ? &task->proc->signal : NULL;
	if (!signal)
		return task->signal.blocked;
	spin_lock_irqsave(&signal->siglock, flags);
	blocked = task->signal.blocked;
	spin_unlock_irqrestore(&signal->siglock, flags);
	return blocked;
}

static void signal_block_mask(struct task_struct *task, uint64_t mask)
{
	struct signal_struct *signal;
	irq_flags_t flags;

	if (!task)
		return;
	signal = task->proc ? &task->proc->signal : NULL;
	mask &= SIGNAL_STANDARD_MASK & ~unblockable_mask();
	if (!signal) {
		task->signal.blocked |= mask;
		return;
	}
	spin_lock_irqsave(&signal->siglock, flags);
	task->signal.blocked |= mask;
	signal_recalc_facts_locked(task, signal);
	spin_unlock_irqrestore(&signal->siglock, flags);
}

static void signal_unblock_mask(struct task_struct *task, uint64_t mask)
{
	struct signal_struct *signal;
	irq_flags_t flags;

	if (!task)
		return;
	mask &= SIGNAL_STANDARD_MASK;
	signal = task->proc ? &task->proc->signal : NULL;
	if (!signal) {
		task->signal.blocked &= ~mask;
		task->signal.blocked &= ~unblockable_mask();
		return;
	}
	spin_lock_irqsave(&signal->siglock, flags);
	task->signal.blocked &= ~mask;
	task->signal.blocked &= ~unblockable_mask();
	signal_recalc_facts_locked(task, signal);
	spin_unlock_irqrestore(&signal->siglock, flags);
}

void sig_set_mask(struct task_struct *task, uint64_t mask)
{
	struct signal_struct *signal;
	irq_flags_t flags;

	if (!task)
		return;
	mask &= SIGNAL_STANDARD_MASK & ~unblockable_mask();
	signal = task->proc ? &task->proc->signal : NULL;
	if (!signal) {
		task->signal.blocked = mask;
		return;
	}
	spin_lock_irqsave(&signal->siglock, flags);
	task->signal.blocked = mask;
	signal_recalc_facts_locked(task, signal);
	spin_unlock_irqrestore(&signal->siglock, flags);
}

bool sig_wait_ready(const struct task_struct *task,
		    const struct task_wait *wait)
{
	struct signal_struct *signal;

	/* The active generation's accepted set is unblocked and its pending
	 * state is included in the same atomic fact bit as ordinary deliverable
	 * signals. Reading the fact bit is intentional: sched_block_current()
	 * calls this while holding wait.lock, so taking siglock here would
	 * invert the global lock order.  The wait mode is consumed by
	 * raise/wake paths to retain and wake ignored accepted signals. */
	(void)wait;
	if (sig_fatal_pending((struct task_struct *)task))
		return true;
	if (!task)
		return false;
	signal = task->proc ? &task->proc->signal : NULL;
	if (signal)
		return atomic_read(&task->signal.has_pending_signal) != 0;
	return (task->signal.pending &
		~(task->signal.blocked & ~unblockable_mask())) != 0;
}

static void signal_clear_pending(struct task_struct *task, uint64_t mask)
{
	struct signal_struct *signal;
	irq_flags_t flags;

	if (!task)
		return;
	signal = task->proc ? &task->proc->signal : NULL;
	if (!signal) {
		signal_clear_pending_locked(task, mask);
		return;
	}
	spin_lock_irqsave(&signal->siglock, flags);
	signal_clear_pending_locked(task, mask);
	signal_recalc_facts_locked(task, signal);
	spin_unlock_irqrestore(&signal->siglock, flags);
}

void sig_defer_mask_restore(struct task_struct *task, uint64_t mask)
{
	if (!task)
		return;

	task->signal.restore_mask = mask & ~unblockable_mask();
	task->signal.restore_mask_pending = true;
}

int sig_send_self(int sig)
{
	siginfo_t info = {
		.si_signo = sig,
		.si_code = SI_KERNEL,
	};

	return send_signal_info_internal(sig, &info, current_task(), false);
}

static void signal_wake_task_callback(struct task_struct *task, void *arg)
{
	wake_signal_target(task, *(const int *)arg);
}

static int send_group_signal_info(int sig, const siginfo_t *info,
				  struct task_struct *leader)
{
	struct signal_struct *signal;
	uint64_t mask;
	irq_flags_t signal_flags;

	if (!sig_valid(sig))
		return -EINVAL;
	if (!info)
		return -EINVAL;
	if (sig == SIGKILL)
		return signal_kill_group(leader, false);
	/* Process-directed delivery must not bail on an exiting leader:
	 * shared pending plus live-sibling wake-up is still valid. Dead
	 * pids are rejected at the pid lookup layer. */

	signal = leader && leader->proc ? &leader->proc->signal : NULL;
	if (!signal)
		return send_signal_info_internal(sig, info, leader, false);

	spin_lock_irqsave(&signal->siglock, signal_flags);
	mask = signal_mask(sig);
	if (signal_group_would_drop_at_raise_locked(leader, sig)) {
		spin_unlock_irqrestore(&signal->siglock, signal_flags);
		return 0;
	}
	if (!(signal->shared_pending & mask)) {
		signal_info_store(&signal->shared_pending_info[sig], sig, info);
		signal->shared_pending |= mask;
	}
	signal_recalc_targets_locked(signal, leader->proc, NULL, 0);
	spin_unlock_irqrestore(&signal->siglock, signal_flags);
	proc_notify_tasks(leader->proc, signal_wake_task_callback, &sig);

	return 0;
}

struct signal_pgrp_send_context {
	int sig;
	const siginfo_t *info;
	bool found;
	bool delivered;
	int first_error;
};

static void signal_pgrp_send_task(struct task_struct *task, void *arg)
{
	struct signal_pgrp_send_context *send = arg;
	int ret;

	send->found = true;
	if (send->sig == 0) {
		send->delivered = true;
		return;
	}
	ret = send_group_signal_info(send->sig, send->info, task);
	if (ret == 0)
		send->delivered = true;
	else if (ret != -ESRCH && send->first_error == 0)
		send->first_error = ret;
}

int sig_send_pgrp(int sig, const siginfo_t *info, struct pgrp_struct *pgrp,
		  struct session_struct *session)
{
	struct signal_pgrp_send_context context = {
		.sig = sig,
		.info = info,
	};

	if (sig != 0 && (!sig_valid(sig) || !info))
		return -EINVAL;
	if (!pgrp || !session || pgrp->session != session)
		return -ESRCH;
	proc_notify_pgrp(pgrp, session, signal_pgrp_send_task, &context);
	if (context.delivered)
		return 0;
	if (!context.found)
		return -ESRCH;
	return context.first_error ? context.first_error : -ESRCH;
}

void sig_notify_parent(const struct proc_parent_event *event)
{
	struct task_struct *target;
	siginfo_t info = {0};

	if (!event || !event->parent || !event->child)
		return;
	wait_channel_wake_all(&event->parent->wait_state.channel);
	if (event->signal == 0)
		return;
	if (!event->child->exit_sigchld_notify)
		return;
	/* SIGCHLD is a process-level notification: deliver through the group
	 * path so a blocked/stopped/exiting leader cannot drop it. */
	target = proc_leader_get(event->parent);
	if (!target)
		return;
	info.si_signo = event->signal;
	info.si_code = event->code;
	info.si_pid = event->pid;
	info.si_uid = event->uid;
	info.si_status = event->status;
	info.si_utime = cputime_to_clock_t(event->cputime.utime_ticks);
	info.si_stime = cputime_to_clock_t(event->cputime.stime_ticks);
	(void)send_group_signal_info(info.si_signo, &info, target);
	task_put(target);
}

int sig_force_info(int sig, const siginfo_t *info, struct task_struct *task)
{
	int ret;

	if (!sig_valid(sig))
		return -EINVAL;
	if (!task)
		return -ESRCH;

	/* A synchronous fault raised while the same signal is blocked cannot be
	 * delivered safely.  Restore the default disposition and terminate rather
	 * than maintaining a kernel shadow stack of userspace signal frames. */
	if (sig_blocked_mask(task) & signal_mask(sig)) {
		if (task == current_task())
			do_exit_signal(sig);
		return -EINTR;
	}
	signal_unblock_mask(task, signal_mask(sig));
	if (task->proc) {
		struct signal_struct *signal = &task->proc->signal;
		irq_flags_t flags;

		spin_lock_irqsave(&signal->siglock, flags);
		if (signal->actions[sig].sa_handler == SIG_IGN)
			signal->actions[sig].sa_handler = SIG_DFL;
		spin_unlock_irqrestore(&signal->siglock, flags);
	}

	ret = send_signal_info_internal(sig, info, task, true);
	if (ret < 0)
		return ret;

	return 0;
}

int sig_wait(uint64_t set, const struct timespec *timeout, siginfo_t *info)
{
	struct wait_deadline deadline;
	struct wait_scope scope __wait_scope = {};
	uint64_t blocked;
	wait_outcome_t outcome;
	int sig;
	int ret;

	if (set & ~SIGNAL_STANDARD_MASK)
		return -EINVAL;

	ret = mtime_deadline_from_timespec(timeout, &deadline);
	if (ret < 0)
		return ret;

	set &= ~unblockable_mask();
	sig = take_pending_from_set(set, info);
	if (sig)
		return sig;
	if (timeout && timeout->tv_sec == 0 && timeout->tv_nsec == 0)
		return -EAGAIN;

	blocked = sig_blocked_mask(current_task());
	/* Make the wait generation visible before unblocking the accepted set.
	 * A signal raised in between is then retained for this synchronous
	 * wait, even when its disposition is SIG_IGN. */
	ret = signal_wait_start_set(current_task(), &scope, &deadline, set);
	if (ret == 0) {
		ret = wait_scope_block(&scope, &outcome);
		wait_scope_complete(&scope);
	}
	sig_set_mask(current_task(), blocked);
	if (ret < 0)
		return ret;

	sig = take_pending_from_set(set, info);
	if (sig)
		return sig;
	if (outcome == WAIT_OUTCOME_TIMEOUT)
		return -EAGAIN;
	BUG_ON(outcome != WAIT_OUTCOME_SIGNAL);
	return -EINTR;
}

void sig_deliver(struct trap_frame *tf)
{
	for (;;) {
		bool shared;
		bool forced;
		siginfo_t info;
		int sig;

		sig = next_signal(&shared);
		if (sig == 0) {
			/* No signal was delivered here, so the interrupted
			 * syscall must not restart: clear any pending restart
			 * context left by a blocked/discarded-signal
			 * interruption (-EINTR on a restartable syscall with no
			 * handler delivery). */
			restart_clear(current_task());
			if (current_task()->signal.restore_mask_pending)
				sig_set_mask(current_task(),
					     signal_take_restore_mask(
						     current_task()));
			return;
		}

		uint64_t mask = signal_mask(sig);
		struct sigaction action = get_signal_action(sig);
		__sighandler_t handler = action.sa_handler;
		struct signal_struct *signal =
			current_task()->proc ? &current_task()->proc->signal
					     : NULL;

		irq_flags_t flags;

		/* Per-task and shared pending, plus the forced attribute, are
		 * consumed under one siglock acquisition so the action and the
		 * pending store stay consistent with the lock-free
		 * next_signal() hint. */
		if (signal)
			spin_lock_irqsave(&signal->siglock, flags);
		if (shared) {
			forced = false;
			if (!(signal && (signal->shared_pending & mask))) {
				if (signal)
					spin_unlock_irqrestore(&signal->siglock,
							       flags);
				continue;
			}
			signal_info_load(&info, sig,
					 &signal->shared_pending_info[sig]);
			signal->shared_pending &= ~mask;
			memset(&signal->shared_pending_info[sig], 0,
			       sizeof(signal->shared_pending_info[sig]));
		} else {
			if (!(current_task()->signal.pending & mask)) {
				if (signal)
					spin_unlock_irqrestore(&signal->siglock,
							       flags);
				continue;
			}
			signal_info_load(&info, sig,
					 &current_task()->signal.pending_info[sig]);
			forced = (current_task()->signal.forced_pending &
				  mask) != 0;
			signal_clear_pending_locked(current_task(), mask);
		}
		if (signal) {
			if (shared)
				signal_recalc_targets_locked(
					signal, current_task()->proc, NULL, 0);
			signal_recalc_facts_locked(current_task(), signal);
		}
		if (signal)
			spin_unlock_irqrestore(&signal->siglock, flags);

		if (handler == SIG_IGN || handler == SIG_DFL)
			(void)restart_for_signal(current_task(), tf, false);
		if (!forced && handler == SIG_DFL &&
		    signal_init_default_ignored(current_task(), sig))
			continue;

		if (sig == SIGKILL)
			do_exit_signal(sig);

		if (handler == SIG_IGN)
			continue;

		if (handler == SIG_DFL) {
			switch (signal_default_action(sig)) {
			case SIGNAL_DEFAULT_IGNORE:
				continue;
			case SIGNAL_DEFAULT_TERMINATE:
				do_exit_signal(sig);
				continue;
			}
		}

		(void)restart_for_signal(current_task(), tf,
					 (action.sa_flags & SA_RESTART) != 0);
		if (setup_signal_frame(tf, sig, &info, &action) < 0)
			do_exit_signal(SIGSEGV);
		return;
	}
}

static void signal_init_user_info(siginfo_t *info, int sig)
{
	struct task_struct *task = current_task();

	*info = (siginfo_t){0};
	info->si_signo = sig;
	info->si_code = SI_USER;
	info->si_pid = task->proc ? task->proc->pid->nr : 0;
	info->si_uid = task_uid(task);
}

int sig_kill(pid_t pid, int sig)
{
	struct task_struct *task;
	siginfo_t info;
	int ret;

	if (sig != 0 && !sig_valid(sig))
		return -EINVAL;

	signal_init_user_info(&info, sig);
	if (pid <= 0)
		return -EINVAL;

	{
		struct proc_struct *proc = pid_lookup_proc(pid);

		if (!proc)
			return -ESRCH;
		task = proc_leader_get(proc);
		proc_put(proc);
	}
	if (!task)
		return -ESRCH;
	if (!signal_may_send_to_task(task, sig)) {
		task_put(task);
		return -EPERM;
	}
	if (sig == 0) {
		task_put(task);
		return 0;
	}

	ret = send_group_signal_info(sig, &info, task);

	task_put(task);
	return ret;
}

int sig_tgkill(pid_t tgid, pid_t tid, int sig)
{
	struct proc_struct *proc;
	struct task_struct *task;
	siginfo_t info;

	if (sig != 0 && !sig_valid(sig))
		return -EINVAL;
	if (tgid <= 0 || tid <= 0)
		return -EINVAL;

	task = pid_lookup_task(tid);
	proc = pid_lookup_proc(tgid);
	if (!task || !proc || !proc_task_is_member(proc, task)) {
		task_put(task);
		proc_put(proc);
		return -ESRCH;
	}
	proc_put(proc);
	if (!signal_may_send_to_task(task, sig)) {
		task_put(task);
		return -EPERM;
	}
	if (sig == 0) {
		task_put(task);
		return 0;
	}

	signal_init_user_info(&info, sig);
	int ret = send_signal_info_internal(sig, &info, task, false);

	task_put(task);
	return ret;
}

int sig_altstack(const struct stack_t *ss, struct stack_t *old_ss)
{
	struct task_struct *task = current_task();
	struct stack_t *sas = &task->signal.sas;

	if (old_ss)
		*old_ss = *sas;

	if (ss) {
		if (sas->ss_flags & SS_ONSTACK)
			return -EPERM;

		if (ss->ss_flags != 0 && ss->ss_flags != SS_DISABLE)
			return -EINVAL;

		if (ss->ss_flags & SS_DISABLE) {
			signal_reset_altstack(task);
		} else {
			if (ss->ss_size < MINSIGSTKSZ)
				return -ENOMEM;
			*sas = *ss;
			sas->ss_flags &= ~SS_ONSTACK;
		}
	}

	return 0;
}

int sig_action(int sig, const struct sigaction *act, struct sigaction *oldact)
{
	const unsigned long supported_flags =
		SA_SIGINFO | SA_ONSTACK | SA_RESTART;
	struct signal_struct *signal =
		current_task()->proc ? &current_task()->proc->signal : NULL;
	struct sigaction kact;
	bool discard_pending = false;
	irq_flags_t flags;

	if (!sig_valid(sig))
		return -EINVAL;
	if (!sig_catchable(sig) && act)
		return -EINVAL;
	if (!signal)
		return -EINVAL;
	if (act) {
		kact = *act;
		if (kact.sa_handler == SIG_ERR)
			return -EINVAL;
		if (kact.sa_flags & ~supported_flags)
			return -EINVAL;
		if (kact.sa_mask & ~SIGNAL_STANDARD_MASK)
			return -EINVAL;
		kact.sa_mask &= ~unblockable_mask();
		discard_pending = kact.sa_handler == SIG_IGN;
	}
	spin_lock_irqsave(&signal->siglock, flags);
	if (oldact)
		*oldact = signal->actions[sig];
	if (act) {
		if (discard_pending)
			signal_discard_ignored_pending_locked(current_task()->proc,
							      sig);
		signal->actions[sig] = kact;
	}
	spin_unlock_irqrestore(&signal->siglock, flags);
	return 0;
}

int sig_procmask(int how, const uint64_t *set, uint64_t *oldset)
{
	struct task_struct *task = current_task();
	uint64_t newset;

	if (oldset)
		*oldset = sig_blocked_mask(task);

	if (!set)
		return 0;
	if (*set & ~SIGNAL_STANDARD_MASK)
		return -EINVAL;

	newset = *set & ~unblockable_mask();

	switch (how) {
	case SIG_BLOCK:
		signal_block_mask(task, newset);
		break;
	case SIG_UNBLOCK:
		signal_unblock_mask(task, newset);
		break;
	case SIG_SETMASK:
		sig_set_mask(task, newset);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/*
 * Syscall-restart context: restart state is task-owned, assigned by the
 * signal module.
 * signal state; the signal module owns the policy and the dispatcher only
 * saves through these interfaces.
 */
void restart_save(struct task_struct *task, const struct trap_frame *tf,
		  size_t nr)
{
	struct restart_context *context = &task->restart;

	context->pc = trap_user_pc(tf) - 4;
	for (uint32_t index = 0; index < 6; index++)
		context->args[index] = sysarg(tf, index);
	context->nr = nr;
	context->valid = true;
	context->restartable = signal_restartable(nr);
}

void restart_clear(struct task_struct *task)
{
	if (task)
		memset(&task->restart, 0, sizeof(task->restart));
}

void restart_finish(struct task_struct *task, ssize_t ret)
{
	if (ret != -EINTR || !task->restart.restartable)
		restart_clear(task);
}

bool restart_for_signal(struct task_struct *task, struct trap_frame *tf,
			bool sa_restart)
{
	struct restart_context *context;

	if (!task)
		return false;
	context = &task->restart;
	if (!context->valid)
		return false;
	if (!sa_restart || !context->restartable) {
		restart_clear(task);
		return false;
	}

	trap_set_user_pc(tf, context->pc);
	trap_set_arg0(tf, context->args[0]);
	tf->a1 = context->args[1];
	tf->a2 = context->args[2];
	tf->a3 = context->args[3];
	tf->a4 = context->args[4];
	tf->a5 = context->args[5];
	tf->a7 = context->nr;
	restart_clear(task);
	return true;
}

ssize_t sig_return(struct trap_frame *tf, uintptr_t sp)
{
	uint64_t mask;
	struct stack_t stack;

	if (arch_signal_restore(tf, sp, &mask, &stack) < 0)
		do_exit_signal(SIGSEGV);
	sig_set_mask(current_task(), mask);
	current_task()->signal.sas = stack;
	return (ssize_t)trap_return_value(tf);
}
