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
#include <nuvix/mutex.h>
#include <nuvix/pid.h>
#include <nuvix/printk.h>
#include <nuvix/proc.h>
#include <nuvix/refcount.h>
#include <nuvix/sched.h>
#include <nuvix/rseq.h>
#include <nuvix/signal.h>
#include <nuvix/slab.h>
#include <nuvix/task.h>
#include <nuvix/syscall.h>
#include <nuvix/time.h>
#include <nuvix/user_map.h>
#include <nuvix/wait.h>
#include <uapi/futex.h>
#include <uapi/syscall.h>
#include <nuvix/processor.h>
#include <nuvix/page.h>
#include <nuvix/pgtable.h>
#include <nuvix/trap.h>

#define RISCV_REG_ZERO 0
#define RISCV_REG_A7   17
#define RISCV_OP_IMM   0x13
#define RISCV_ECALL    0x00000073
#define RISCV_J_SELF   0x0000006f

#define RISCV_ADDI(rd, rs1, imm)                                               \
	((((uint32_t)(imm) & 0xfff) << 20) | ((uint32_t)(rs1) << 15) |         \
	 ((uint32_t)(rd) << 7) | RISCV_OP_IMM)

#define SIGNAL_STANDARD_MASK   ((1UL << (SIGRTMIN - 1)) - 1)
#define SIGNAL_TRAMPOLINE_ADDR (USER_STACK_GUARD_BASE - PAGE_SIZE)

static_assert(SYS_rt_sigreturn >= 0 && SYS_rt_sigreturn < 2048,
	      "SYS_sigreturn must fit in a RISC-V addi immediate");

struct signal_frame_state {
	uintptr_t sp;
	int sig;
	struct signal_frame_state *previous;
};

struct sighand_struct {
	refcount_t refcount;
	mutex_t lock;
	struct sigaction sigactions[NSIG + 1];
};

struct signal_struct {
	refcount_t refcount;
	/* One per-Proc irq-safe spinlock guards pending, blocked masks, stop
	 * state, and fact bits. Handler-table accesses use sighand->lock. The
	 * lock order is sighand->lock -> siglock -> task.wait.lock for signal
	 * admission. Other signal paths never take siglock while holding
	 * wait.lock. */
	spinlock_t siglock;
	uint64_t shared_pending;
	siginfo_t shared_pending_info[NSIG + 1];
	/* A default stop affects the whole thread group. Members stop at their
	 * next user-return or wait recheck; SIGCONT clears the state. */
	bool group_stopped;
	int group_stop_sig;
};

struct rt_sigframe {
	siginfo_t info;
	struct ucontext uc;
};

static_assert(sizeof(struct rt_sigframe) == 1088,
	      "Linux riscv64 rt_sigframe size mismatch");

enum signal_default_action {
	SIGNAL_DEFAULT_TERMINATE,
	SIGNAL_DEFAULT_IGNORE,
	SIGNAL_DEFAULT_STOP,
	SIGNAL_DEFAULT_CONTINUE,
};

static void *trampoline_page;

static bool task_wait_accepts_signal(struct task_struct *task, int sig);
static void signal_recalc_facts_locked(struct task_struct *task,
				       struct signal_struct *signal);
static bool signal_group_stop_clear(struct task_struct *task);
static void signal_clear_frames(struct task_struct *task);
static uint64_t signal_mask(int sig);
static uint64_t unblockable_mask(void);
static void signal_reset_altstack(struct task_struct *task);
static void signal_block_mask(struct task_struct *task, uint64_t mask);
static void signal_unblock_mask(struct task_struct *task, uint64_t mask);
static void signal_clear_pending(struct task_struct *task, uint64_t mask);
static int send_group_signal_info(int sig, const siginfo_t *info,
				  struct task_struct *leader);

static void signal_task_snapshot_release(struct task_struct **tasks,
					 size_t count)
{
	for (size_t index = 0; index < count; index++)
		task_put(tasks[index]);
	kfree(tasks);
}

static int signal_frame_clone(struct task_struct *child,
			      const struct task_struct *parent)
{
	const struct signal_frame_state *source;
	struct signal_frame_state **destination = &child->signal.signal_frames;

	for (source = parent->signal.signal_frames; source;
	     source = source->previous) {
		struct signal_frame_state *copy =
			kmalloc(sizeof(*copy), ALLOC_NOWAIT);

		if (!copy) {
			signal_clear_frames(child);
			return -ENOMEM;
		}
		copy->sp = source->sp;
		copy->sig = source->sig;
		copy->previous = NULL;
		*destination = copy;
		destination = &copy->previous;
	}

	return 0;
}

static struct signal_frame_state *signal_frame_alloc(uintptr_t sp, int sig)
{
	struct signal_frame_state *state =
		kmalloc(sizeof(*state), ALLOC_NOWAIT);

	if (!state)
		return NULL;
	state->sp = sp;
	state->sig = sig;
	state->previous = NULL;
	return state;
}

static void signal_frame_push(struct task_struct *task,
			      struct signal_frame_state *state)
{
	state->previous = task->signal.signal_frames;
	task->signal.signal_frames = state;
}

static int signal_frame_sp(uintptr_t top, uintptr_t floor, uintptr_t *sp)
{
	uintptr_t frame_sp;

	if (check_sub_overflow(top, (uintptr_t)sizeof(struct rt_sigframe),
			       &frame_sp))
		return -EFAULT;
	frame_sp &= ~(uintptr_t)0xf;
	if (frame_sp < floor)
		return -EFAULT;
	*sp = frame_sp;
	return 0;
}

static void signal_frame_pop(struct task_struct *task)
{
	struct signal_frame_state *state = task->signal.signal_frames;

	BUG_ON(!state);
	task->signal.signal_frames = state->previous;
	kfree(state);
}

static bool signal_frame_contains(const struct task_struct *task, int sig)
{
	const struct signal_frame_state *state = task->signal.signal_frames;

	for (; state; state = state->previous) {
		if (state->sig == sig)
			return true;
	}
	return false;
}

static bool signal_is_stop_signal(int sig)
{
	return sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN ||
	       sig == SIGTTOU;
}

static uint64_t signal_stop_mask(void)
{
	return signal_mask(SIGSTOP) | signal_mask(SIGTSTP) |
	       signal_mask(SIGTTIN) | signal_mask(SIGTTOU);
}

static enum signal_default_action signal_default_action(int sig)
{
	switch (sig) {
	case SIGCHLD:
	case SIGURG:
	case SIGWINCH:
		return SIGNAL_DEFAULT_IGNORE;
	case SIGCONT:
		return SIGNAL_DEFAULT_CONTINUE;
	case SIGSTOP:
	case SIGTSTP:
	case SIGTTIN:
	case SIGTTOU:
		return SIGNAL_DEFAULT_STOP;
	default:
		return SIGNAL_DEFAULT_TERMINATE;
	}
}

static void signal_recalc_task_callback(struct task_struct *task, void *arg)
{
	signal_recalc_facts_locked(task, arg);
}

static struct sighand_struct *sighand_alloc(void)
{
	struct sighand_struct *sighand =
		kmalloc(sizeof(*sighand), ALLOC_NOWAIT);

	if (!sighand)
		return NULL;

	memset(sighand, 0, sizeof(*sighand));
	refcount_set(&sighand->refcount, 1);
	mutex_init(&sighand->lock, LOCK_RANK_SIGNAL_HAND, LOCK_IRQ_TASK_ONLY);
	return sighand;
}

static struct sighand_struct *sighand_dup(struct sighand_struct *old)
{
	struct sighand_struct *sighand = sighand_alloc();

	if (!sighand)
		return NULL;
	if (!old)
		return sighand;

	mutex_lock(&old->lock);
	memcpy(sighand->sigactions, old->sigactions,
	       sizeof(sighand->sigactions));
	mutex_unlock(&old->lock);
	return sighand;
}

static void sighand_get(struct sighand_struct *sighand)
{
	if (sighand)
		refcount_inc(&sighand->refcount);
}

static void sighand_put(struct sighand_struct *sighand)
{
	if (!sighand)
		return;

	if (refcount_dec_and_test(&sighand->refcount))
		kfree(sighand);
}

static struct signal_struct *signal_state_alloc(void)
{
	struct signal_struct *signal = kmalloc(sizeof(*signal), ALLOC_NOWAIT);

	if (!signal)
		return NULL;

	memset(signal, 0, sizeof(*signal));
	refcount_set(&signal->refcount, 1);
	spin_lock_init(&signal->siglock, LOCK_RANK_SIGNAL_SHARED,
		       LOCK_IRQ_TASK_ONLY);
	return signal;
}

static void signal_state_put(struct signal_struct *signal)
{
	if (!signal)
		return;

	if (refcount_dec_and_test(&signal->refcount))
		kfree(signal);
}

/* Reads the shared handler table under sighand->lock. */
static __sighandler_t signal_handler_for_task_locked(struct task_struct *task,
						     int sig)
{
	struct sighand_struct *sighand =
		task && task->proc ? task->proc->sighand : NULL;

	if (!sighand)
		return SIG_DFL;
	return sighand->sigactions[sig].sa_handler;
}

static __sighandler_t signal_handler_for_task(struct task_struct *task, int sig)
{
	struct sighand_struct *sighand =
		task && task->proc ? task->proc->sighand : NULL;
	__sighandler_t handler = SIG_DFL;

	if (!sighand)
		return handler;

	mutex_lock(&sighand->lock);
	handler = signal_handler_for_task_locked(task, sig);
	mutex_unlock(&sighand->lock);
	return handler;
}

static struct sigaction signal_sigchld_action(const struct proc_struct *proc)
{
	struct sigaction action = {0};
	struct sighand_struct *sighand;

	if (!proc)
		return action;
	sighand = proc->sighand;
	if (!sighand)
		return action;
	mutex_lock(&sighand->lock);
	action = sighand->sigactions[SIGCHLD];
	mutex_unlock(&sighand->lock);
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
	return action == SIGNAL_DEFAULT_TERMINATE ||
	       action == SIGNAL_DEFAULT_STOP;
}

static bool signal_init_default_ignored(struct task_struct *task, int sig)
{
	enum signal_default_action action;

	if (!init_process_is_task(task) ||
	    signal_handler_for_task(task, sig) != SIG_DFL)
		return false;

	action = signal_default_action(sig);
	return action == SIGNAL_DEFAULT_TERMINATE ||
	       action == SIGNAL_DEFAULT_STOP;
}

/* Raise-time drop predicate: an unblocked notification whose disposition at
 * raise time is ignored (explicit SIG_IGN, or a SIG_DFL default-ignore such
 * as SIGCHLD/SIGURG/SIGWINCH, or suppressed for init) is dropped immediately
 * — never queued, never woken.  Blocked signals are never dropped, and neither
 * are signals this task is synchronously waiting for (sigtimedwait's accepted
 * set): both must stay queued so a later unblock or the wait can consume them
 * even under a default-ignore disposition.  Forced/exception and stop/continue
 * signals are never dropped.  Installing SIG_IGN later discards existing
 * non-forced pending instances.  Callers hold the thread-group siglock. */
static bool signal_would_drop_at_raise_locked(struct task_struct *task, int sig,
					      bool forced)
{
	__sighandler_t handler;

	if (forced)
		return false;
	if (signal_is_stop_signal(sig) || sig == SIGCONT)
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

static void sighand_reset_for_exec(struct sighand_struct *sighand)
{
	if (!sighand)
		return;
	mutex_lock(&sighand->lock);
	for (int sig = 1; sig <= NSIG; sig++) {
		struct sigaction *action = &sighand->sigactions[sig];

		if (action->sa_handler != SIG_IGN)
			memset(action, 0, sizeof(*action));
	}
	mutex_unlock(&sighand->lock);
}

static bool task_wait_accepts_signal(struct task_struct *task, int sig)
{
	bool accepts = false;
	irq_flags_t flags;

	if (!task || !sig_valid(sig))
		return false;
	spin_lock_irqsave(&task->wait.lock, &flags);
	accepts = task->wait.status == WAIT_ACTIVE &&
		  task->wait.signal_mode == TASK_WAIT_SIGNAL_SET &&
		  (task->wait.signal_set & signal_mask(sig)) != 0;
	spin_unlock_irqrestore(&task->wait.lock, flags);
	return accepts;
}

/* Start a sigtimedwait generation and remove its accepted signals from the
 * blocked mask as one signal-state transaction.  Signal senders take the same
 * siglock, so a raise cannot observe an active wait with the old mask or the
 * new mask half-published. */
static int signal_wait_start_set(struct task_struct *task,
				 const struct wait_deadline *deadline,
				 uint64_t signal_set)
{
	struct signal_struct *signal;
	irq_flags_t flags;
	int ret;

	if (!task)
		return -EINVAL;
	signal = task->proc ? task->proc->signal : NULL;
	if (!signal) {
		ret = wait_start_signal_set(&task->wait,
					    WAIT_FLAG_INTERRUPTIBLE, deadline,
					    signal_set);
		if (ret == 0)
			task->signal.blocked &= ~signal_set;
		return ret;
	}

	spin_lock_irqsave(&signal->siglock, &flags);
	ret = wait_start_signal_set_locked(&task->wait, WAIT_FLAG_INTERRUPTIBLE,
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

	for (int sig = 1; sig <= NSIG; sig++) {
		if (mask & signal_mask(sig))
			memset(&task->signal.pending_info[sig], 0,
			       sizeof(task->signal.pending_info[sig]));
	}
	task->signal.pending &= ~mask;
	task->signal.forced_pending &= ~mask;
}

static void signal_discard_ignored_pending_locked(struct signal_struct *signal,
						  struct task_struct **targets,
						  size_t count, int sig)
{
	uint64_t mask = signal_mask(sig);

	if (!signal)
		return;
	if (signal->shared_pending & mask) {
		memset(&signal->shared_pending_info[sig], 0,
		       sizeof(signal->shared_pending_info[sig]));
		signal->shared_pending &= ~mask;
	}
	for (size_t index = 0; index < count; index++) {
		struct task_struct *task = targets[index];

		if (task->signal.forced_pending & mask)
			continue;
		if (task->signal.pending & mask) {
			memset(&task->signal.pending_info[sig], 0,
			       sizeof(task->signal.pending_info[sig]));
			task->signal.pending &= ~mask;
		}
		signal_recalc_facts_locked(task, signal);
	}
}

static void signal_clear_opposite_pending_locked(struct task_struct *task,
						 int sig)
{
	if (sig == SIGCONT)
		signal_clear_pending_locked(task, signal_stop_mask());
	else if (signal_is_stop_signal(sig))
		signal_clear_pending_locked(task, signal_mask(SIGCONT));
}

static void
signal_clear_opposite_pending_task_callback(struct task_struct *task, void *arg)
{
	signal_clear_opposite_pending_locked(task, *(const int *)arg);
}

static void signal_clear_shared_opposite_pending(struct signal_struct *signal,
						 int sig)
{
	uint64_t mask = 0;

	if (sig == SIGCONT)
		mask = signal_stop_mask();
	else if (signal_is_stop_signal(sig))
		mask = signal_mask(SIGCONT);

	for (int pending_sig = 1; pending_sig <= NSIG; pending_sig++) {
		if (mask & signal_mask(pending_sig))
			memset(&signal->shared_pending_info[pending_sig], 0,
			       sizeof(signal->shared_pending_info
					      [pending_sig]));
	}
	signal->shared_pending &= ~mask;
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
	enum task_wait_policy policy;
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
	signal = task->proc ? task->proc->signal : NULL;
	if (signal) {
		spin_lock_irqsave(&signal->siglock, &flags);
		blocked = task->signal.blocked;
		pending = task->signal.pending;
		forced = task->signal.forced_pending;
		spin_unlock_irqrestore(&signal->siglock, flags);
	} else {
		blocked = task->signal.blocked;
		pending = task->signal.pending;
		forced = task->signal.forced_pending;
	}
	spin_lock_irqsave(&task->wait.lock, &flags);
	policy = task->wait.policy;
	accepted = task->wait.signal_mode == TASK_WAIT_SIGNAL_SET &&
		   (task->wait.signal_set & mask) != 0;
	spin_unlock_irqrestore(&task->wait.lock, flags);

	fatal = sig == SIGKILL ||
		((pending | forced) & signal_mask(SIGKILL)) != 0;
	deliverable =
		fatal || accepted || !sig_catchable(sig) || !(blocked & mask);
	if (deliverable && policy != TASK_WAIT_UNINTERRUPTIBLE)
		(void)wait_wake_signal(task, fatal);
	if (sig == SIGCONT || sig == SIGKILL)
		(void)sched_resume(task);
}

static int send_signal_info_internal(int sig, const siginfo_t *info,
				     struct task_struct *task, bool force)
{
	uint64_t mask;
	struct signal_struct *signal;
	struct sighand_struct *sighand;
	struct task_struct **targets = NULL;
	size_t target_count = 0;
	irq_flags_t flags;

	if (!sig_valid(sig))
		return -EINVAL;
	if (!info)
		return -EINVAL;
	if (!task || task_is_exiting(task))
		return -ESRCH;

	mask = signal_mask(sig);
	signal = task->proc ? task->proc->signal : NULL;
	sighand = task->proc ? task->proc->sighand : NULL;
	if (signal && (sig == SIGCONT || signal_is_stop_signal(sig)))
		targets = kmalloc_array(PID_COUNT, sizeof(*targets),
					ALLOC_NOWAIT);
	if (targets && task->proc)
		target_count = proc_task_snapshot(task->proc, NULL, targets,
						  PID_COUNT);
	if (sighand)
		mutex_lock(&sighand->lock);
	if (signal)
		spin_lock_irqsave(&signal->siglock, &flags);
	/* Dropped at raise: an unblocked, ignored (SIG_IGN / default-ignore /
	 * init-suppressed) notification is neither queued nor woken.  Blocked
	 * signals are never dropped.  Exception/forced and stop/continue
	 * signals are never dropped. */
	if (!(task->signal.blocked & mask) &&
	    signal_would_drop_at_raise_locked(task, sig, force)) {
		if (signal)
			spin_unlock_irqrestore(&signal->siglock, flags);
		if (sighand)
			mutex_unlock(&sighand->lock);
		kfree(targets);
		return 0;
	}
	if (signal)
		spin_lock(&task->wait.lock);
	else
		spin_lock_irqsave(&task->wait.lock, &flags);
	/* This is the authoritative admission check: exit and task-directed
	 * queuing hold the same signal -> wait lock pair while changing state. */
	if (task_is_exiting(task)) {
		if (signal) {
			spin_unlock(&task->wait.lock);
			spin_unlock_irqrestore(&signal->siglock, flags);
		} else {
			spin_unlock_irqrestore(&task->wait.lock, flags);
		}
		if (sighand)
			mutex_unlock(&sighand->lock);
		kfree(targets);
		return -ESRCH;
	}
	/* A failed snapshot uses the topology walk fallback. Keep siglock while
	 * dropping wait.lock: exit admission takes siglock first, so the check
	 * remains authoritative without inverting wait.lock -> topology.lock. */
	if (sig == SIGCONT && signal && task->proc && !targets) {
		spin_unlock(&task->wait.lock);
		proc_for_each_task(task->proc,
				  signal_clear_opposite_pending_task_callback,
				  &sig);
		spin_lock(&task->wait.lock);
		if (task_is_exiting(task)) {
			spin_unlock(&task->wait.lock);
			spin_unlock_irqrestore(&signal->siglock, flags);
			if (sighand)
				mutex_unlock(&sighand->lock);
			return -ESRCH;
		}
	}
	if (signal)
		signal_clear_shared_opposite_pending(signal, sig);
	if (sig == SIGCONT && signal && task->proc) {
		if (targets) {
			for (size_t index = 0; index < target_count; index++)
				signal_clear_opposite_pending_locked(
					targets[index], sig);
		}
	} else if (sig != SIGCONT || !signal || !task->proc) {
		signal_clear_opposite_pending_locked(task, sig);
	}
	if (!(task->signal.pending & mask)) {
		task->signal.pending_info[sig] = *info;
		task->signal.pending_info[sig].si_signo = sig;
		task->signal.pending |= mask;
	}
	if (force)
		task->signal.forced_pending |= mask;
	if (signal) {
		signal_recalc_facts_locked(task, signal);
		spin_unlock(&task->wait.lock);
		signal_recalc_targets_locked(signal, task->proc, targets,
					     target_count);
		spin_unlock_irqrestore(&signal->siglock, flags);
	} else
		spin_unlock_irqrestore(&task->wait.lock, flags);
	if (sighand)
		mutex_unlock(&sighand->lock);
	signal_task_snapshot_release(targets, target_count);
	wake_signal_target(task, sig);
	if (sig == SIGCONT) {
		(void)signal_group_stop_clear(task);
		(void)sched_resume(task);
		if (task->proc) {
			struct proc_parent_event event;

			proc_publish_continue(task->proc, &event);
			sig_notify_parent(&event);
			proc_parent_event_release(&event);
		}
	}

	return 0;
}

static bool
signal_group_would_drop_at_raise_locked(struct task_struct *leader, int sig,
					struct task_struct **targets,
					size_t count)
{
	__sighandler_t handler;
	uint64_t mask;

	if (!leader || signal_is_stop_signal(sig) || sig == SIGCONT)
		return false;
	mask = signal_mask(sig);
	handler = signal_handler_for_task_locked(leader, sig);
	if (handler != SIG_IGN && handler != SIG_DFL)
		return false;
	for (size_t index = 0; index < count; index++) {
		struct task_struct *task = targets[index];

		if (task_wait_accepts_signal(task, sig))
			return false;
		if (!(task->signal.blocked & mask))
			return handler == SIG_IGN ||
			       signal_init_default_ignored_locked(task, sig) ||
			       signal_default_action(sig) ==
				       SIGNAL_DEFAULT_IGNORE;
	}
	return false;
}

static bool signal_same_session(const struct task_struct *sender,
				const struct task_struct *target)
{
	pid_t sender_pgid;
	pid_t sender_sid;
	pid_t target_pgid;
	pid_t target_sid;

	if (!sender || !target || !sender->proc || !target->proc)
		return false;
	if (proc_snapshot_topology(sender->proc, &sender_pgid, &sender_sid) <
		    0 ||
	    proc_snapshot_topology(target->proc, &target_pgid, &target_sid) < 0)
		return false;
	return sender_sid == target_sid;
}

static bool signal_may_send_to_task(const struct task_struct *target, int sig)
{
	const struct task_struct *sender = current_task();
	const struct cred *sender_cred;
	const struct cred *target_cred;

	if (!sender || !target || !sender->cred || !target->cred)
		return false;
	sender_cred = sender->cred;
	target_cred = target->cred;
	if (sender_cred->euid == 0 || sender_cred->euid == target_cred->ruid ||
	    sender_cred->euid == target_cred->suid ||
	    sender_cred->ruid == target_cred->ruid ||
	    sender_cred->ruid == target_cred->suid)
		return true;
	return sig == SIGCONT && signal_same_session(sender, target);
}

static int signal_send_user_pgrp_snapshot(int sig, const siginfo_t *info,
					  struct pgrp_struct *pgrp,
					  struct session_struct *session)
{
	bool found = false;
	bool permitted = false;
	bool delivered = false;
	int first_error = 0;
	struct task_struct **tasks;
	size_t count;
	size_t index;

	if (sig != 0 && (!sig_valid(sig) || !info))
		return -EINVAL;
	if (!pgrp || !session || pgrp->session != session)
		return -ESRCH;
	tasks = kmalloc_array(PID_COUNT, sizeof(*tasks), ALLOC_NOWAIT);
	if (!tasks)
		return -ENOMEM;
	count = proc_pgrp_task_snapshot(pgrp, session, tasks, PID_COUNT);
	for (index = 0; index < count; index++) {
		int ret;

		found = true;
		if (!signal_may_send_to_task(tasks[index], sig)) {
			task_put(tasks[index]);
			continue;
		}
		permitted = true;
		if (sig == 0) {
			delivered = true;
			task_put(tasks[index]);
			continue;
		}
		ret = send_group_signal_info(sig, info, tasks[index]);
		if (ret == 0)
			delivered = true;
		else if (ret != -ESRCH && first_error == 0)
			first_error = ret;
		task_put(tasks[index]);
	}
	kfree(tasks);
	if (delivered)
		return 0;
	if (!found)
		return -ESRCH;
	if (!permitted)
		return -EPERM;
	return first_error ? first_error : -ESRCH;
}

static int send_pgrp_signal_info(int sig, const siginfo_t *info, pid_t pgid,
				 pid_t sid)
{
	struct pgrp_struct *pgrp;
	struct session_struct *session;
	int ret;

	if (pgid <= 0 || sid < 0)
		return -ESRCH;
	pgrp = proc_lookup_pgrp(pgid);
	if (!pgrp)
		return -ESRCH;
	session = pgrp->session;
	if (!session) {
		proc_pgrp_put(pgrp);
		return -ESRCH;
	}
	if (!proc_session_try_get(session)) {
		proc_pgrp_put(pgrp);
		return -ESRCH;
	}
	if (sid != 0 && (!session->sid || session->sid->nr != sid)) {
		proc_session_put(session);
		proc_pgrp_put(pgrp);
		return -ESRCH;
	}
	ret = signal_send_user_pgrp_snapshot(sig, info, pgrp, session);
	proc_session_put(session);
	proc_pgrp_put(pgrp);
	return ret;
}

static int signal_map_trampoline(pte_t *pgd)
{
	static const uint32_t code[] = {
		RISCV_ADDI(RISCV_REG_A7, RISCV_REG_ZERO, SYS_rt_sigreturn),
		RISCV_ECALL,
		RISCV_J_SELF,
	};

	if (!trampoline_page) {
		trampoline_page = get_free_page(0, ALLOC_NOWAIT);
		if (!trampoline_page)
			return -ENOMEM;
		memset(trampoline_page, 0, PAGE_SIZE);
		memcpy(trampoline_page, code, sizeof(code));
		flush_icache();
	}

	return map_page(pgd, SIGNAL_TRAMPOLINE_ADDR,
			__pa((uintptr_t)trampoline_page),
			pgprot_user(true, false, true));
}

/* Mark the whole thread group stopped and stop every live sibling.  A
 * delivered stop default action (SIGSTOP, or a TSTP/TTIN/TTOU at SIG_DFL)
 * stops the entire thread group, not just the delivering thread (user story
 * 6).  Blocked members that cannot be stopped synchronously self-stop at the
 * next return to user via signal_group_stopped(). */
static void signal_group_stop_task_callback(struct task_struct *task, void *arg)
{
	if (task != arg)
		(void)sched_stop(task);
}

static void signal_group_resume_task_callback(struct task_struct *task,
					      void *arg)
{
	(void)arg;
	(void)sched_resume(task);
}

static bool signal_group_stop_begin(struct task_struct *task, int sig)
{
	struct signal_struct *signal =
		task && task->proc ? task->proc->signal : NULL;
	irq_flags_t flags;
	bool transitioned;

	if (!signal)
		return false;
	spin_lock_irqsave(&signal->siglock, &flags);
	transitioned = !signal->group_stopped;
	if (transitioned) {
		signal->group_stopped = true;
		signal->group_stop_sig = sig;
	}
	spin_unlock_irqrestore(&signal->siglock, flags);
	if (transitioned)
		proc_for_each_task(task->proc, signal_group_stop_task_callback,
				   task);
	return transitioned;
}

/* Clear the whole-group stop state and resume every stopped member.  Runs
 * under siglock, mirroring the group_stopped transitions made by
 * signal_group_stop_begin(). */
static bool signal_group_stop_clear(struct task_struct *task)
{
	struct signal_struct *signal =
		task && task->proc ? task->proc->signal : NULL;
	irq_flags_t flags;
	bool transitioned;

	if (!signal)
		return false;
	spin_lock_irqsave(&signal->siglock, &flags);
	transitioned = signal->group_stopped;
	if (!transitioned) {
		spin_unlock_irqrestore(&signal->siglock, flags);
		return false;
	}
	signal->group_stopped = false;
	signal->group_stop_sig = 0;
	spin_unlock_irqrestore(&signal->siglock, flags);
	proc_for_each_task(task->proc, signal_group_resume_task_callback, NULL);
	return true;
}

static void stop_current(int sig, bool group_directed)
{
	struct proc_parent_event event;

	if (group_directed)
		(void)signal_group_stop_begin(current_task(), sig);
	(void)sched_stop(current_task());
	if (current_task()->proc) {
		proc_publish_stop(current_task()->proc, sig, &event);
		sig_notify_parent(&event);
		proc_parent_event_release(&event);
	}
	if (irqs_disabled())
		schedule_irqoff();
	else
		schedule();
}

static void signal_save_user_regs(struct user_regs_struct *regs,
				  const struct trap_frame *tf)
{
	regs->pc = tf->sepc;
	regs->ra = tf->ra;
	regs->sp = tf->sp;
	regs->gp = tf->gp;
	regs->tp = tf->tp;
	regs->t0 = tf->t0;
	regs->t1 = tf->t1;
	regs->t2 = tf->t2;
	regs->s0 = tf->s0;
	regs->s1 = tf->s1;
	regs->a0 = tf->a0;
	regs->a1 = tf->a1;
	regs->a2 = tf->a2;
	regs->a3 = tf->a3;
	regs->a4 = tf->a4;
	regs->a5 = tf->a5;
	regs->a6 = tf->a6;
	regs->a7 = tf->a7;
	regs->s2 = tf->s2;
	regs->s3 = tf->s3;
	regs->s4 = tf->s4;
	regs->s5 = tf->s5;
	regs->s6 = tf->s6;
	regs->s7 = tf->s7;
	regs->s8 = tf->s8;
	regs->s9 = tf->s9;
	regs->s10 = tf->s10;
	regs->s11 = tf->s11;
	regs->t3 = tf->t3;
	regs->t4 = tf->t4;
	regs->t5 = tf->t5;
	regs->t6 = tf->t6;
}

static void signal_restore_user_regs(struct trap_frame *tf,
				     const struct user_regs_struct *regs)
{
	tf->sepc = regs->pc;
	tf->ra = regs->ra;
	tf->sp = regs->sp;
	tf->gp = regs->gp;
	tf->tp = regs->tp;
	tf->t0 = regs->t0;
	tf->t1 = regs->t1;
	tf->t2 = regs->t2;
	tf->s0 = regs->s0;
	tf->s1 = regs->s1;
	tf->a0 = regs->a0;
	tf->a1 = regs->a1;
	tf->a2 = regs->a2;
	tf->a3 = regs->a3;
	tf->a4 = regs->a4;
	tf->a5 = regs->a5;
	tf->a6 = regs->a6;
	tf->a7 = regs->a7;
	tf->s2 = regs->s2;
	tf->s3 = regs->s3;
	tf->s4 = regs->s4;
	tf->s5 = regs->s5;
	tf->s6 = regs->s6;
	tf->s7 = regs->s7;
	tf->s8 = regs->s8;
	tf->s9 = regs->s9;
	tf->s10 = regs->s10;
	tf->s11 = regs->s11;
	tf->t3 = regs->t3;
	tf->t4 = regs->t4;
	tf->t5 = regs->t5;
	tf->t6 = regs->t6;
}

static int setup_signal_frame(struct trap_frame *tf, int sig,
			      const siginfo_t *info,
			      const struct sigaction *action)
{
	uintptr_t sp;
	struct rt_sigframe frame;
	struct signal_frame_state *state;
	struct stack_t *sas = &current_task()->signal.sas;
	bool on_altstack = false;

	memset(&frame, 0, sizeof(frame));

	if ((action->sa_flags & SA_ONSTACK) &&
	    !(sas->ss_flags & (SS_DISABLE | SS_ONSTACK))) {
		uintptr_t top;

		if (check_add_overflow((uintptr_t)sas->ss_sp,
				       (uintptr_t)sas->ss_size, &top) ||
		    signal_frame_sp(top, (uintptr_t)sas->ss_sp, &sp) < 0)
			return -EFAULT;
		on_altstack = true;
	} else {
		if (signal_frame_sp(trap_user_sp(tf), 0, &sp) < 0)
			return -EFAULT;
	}

	if (!access_ok((void *)sp, sizeof(frame)))
		return -EFAULT;

	frame.info = *info;
	frame.info.si_signo = sig;
	frame.uc.uc_flags = 0;
	frame.uc.uc_link = NULL;
	frame.uc.uc_stack = *sas;
	signal_save_user_regs(&frame.uc.uc_mcontext.sc_regs, tf);
	if (current_task()->signal.restore_mask_pending)
		frame.uc.uc_sigmask = signal_take_restore_mask(current_task());
	else
		frame.uc.uc_sigmask = sig_blocked_mask(current_task());

	state = signal_frame_alloc(sp, sig);
	if (!state)
		return -ENOMEM;
	if (copy_to_user((void *)sp, &frame, sizeof(frame)) != 0) {
		kfree(state);
		return -EFAULT;
	}

	if (!(action->sa_flags & SA_NODEFER))
		signal_block_mask(current_task(), signal_mask(sig));
	signal_block_mask(current_task(), action->sa_mask);
	signal_frame_push(current_task(), state);
	if (on_altstack)
		sas->ss_flags |= SS_ONSTACK;

	trap_setup_signal_handler(tf, (uintptr_t)action->sa_handler,
				  SIGNAL_TRAMPOLINE_ADDR, sp, (uintptr_t)sig,
				  sp + offsetof(struct rt_sigframe, info),
				  sp + offsetof(struct rt_sigframe, uc));
	return 0;
}

static uint64_t current_shared_pending(void)
{
	struct task_struct *task = current_task();
	struct signal_struct *signal = task->proc ? task->proc->signal : NULL;
	uint64_t pending = 0;
	irq_flags_t flags;

	if (!signal)
		return 0;

	spin_lock_irqsave(&signal->siglock, &flags);
	pending = signal->shared_pending;
	spin_unlock_irqrestore(&signal->siglock, flags);
	return pending;
}

static int take_pending_from_set(uint64_t set, siginfo_t *info)
{
	struct task_struct *task = current_task();
	struct signal_struct *signal = task->proc ? task->proc->signal : NULL;
	struct task_struct **targets = NULL;
	size_t target_count = 0;
	irq_flags_t flags;
	int sig;

	if (!signal) {
		uint64_t pending = task->signal.pending & set;

		for (sig = 1; sig < SIGRTMIN; sig++) {
			uint64_t mask = signal_mask(sig);

			if (!(pending & mask))
				continue;
			*info = task->signal.pending_info[sig];
			signal_clear_pending_locked(task, mask);
			return sig;
		}
		return 0;
	}

	if (signal && task->proc) {
		targets = kmalloc_array(PID_COUNT, sizeof(*targets),
					ALLOC_NOWAIT);
		if (targets)
			target_count = proc_task_snapshot(task->proc, NULL,
							  targets, PID_COUNT);
	}

	/* Per-task and shared pending are drained under one siglock
	 * acquisition, so no group member can double-consume a shared signal.
	 */
	spin_lock_irqsave(&signal->siglock, &flags);
	for (sig = 1; sig < SIGRTMIN; sig++) {
		uint64_t mask = signal_mask(sig);

		if (!(task->signal.pending & set & mask))
			continue;
		*info = task->signal.pending_info[sig];
		signal_clear_pending_locked(task, mask);
		signal_recalc_facts_locked(task, signal);
		spin_unlock_irqrestore(&signal->siglock, flags);
		signal_task_snapshot_release(targets, target_count);
		return sig;
	}
	for (sig = 1; sig < SIGRTMIN; sig++) {
		uint64_t mask = signal_mask(sig);

		if (!(signal->shared_pending & set & mask))
			continue;
		*info = signal->shared_pending_info[sig];
		signal->shared_pending &= ~mask;
		memset(&signal->shared_pending_info[sig], 0,
		       sizeof(signal->shared_pending_info[sig]));
		signal_recalc_targets_locked(signal, task->proc, targets,
					     target_count);
		signal_recalc_facts_locked(task, signal);
		spin_unlock_irqrestore(&signal->siglock, flags);
		signal_task_snapshot_release(targets, target_count);
		return sig;
	}
	spin_unlock_irqrestore(&signal->siglock, flags);
	signal_task_snapshot_release(targets, target_count);
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
	struct sighand_struct *sighand =
		task->proc ? task->proc->sighand : NULL;
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	if (!sighand)
		return action;
	mutex_lock(&sighand->lock);
	action = sighand->sigactions[sig];
	mutex_unlock(&sighand->lock);
	return action;
}

static void reset_signal_action(int sig)
{
	struct task_struct *task = current_task();
	struct sighand_struct *sighand =
		task->proc ? task->proc->sighand : NULL;

	if (!sighand)
		return;
	mutex_lock(&sighand->lock);
	memset(&sighand->sigactions[sig], 0, sizeof(sighand->sigactions[sig]));
	mutex_unlock(&sighand->lock);
}

static int kill_all_processes(int sig, const siginfo_t *info)
{
	bool found = false;
	bool permitted = false;
	bool delivered = false;
	int first_error = 0;
	pid_t caller_tgid = current_task()->proc->pid->nr;

	for (pid_t nr = 1; nr <= PID_MAX; nr++) {
		struct task_struct *task = pid_lookup_task(nr);
		int ret;

		if (!task || !task->proc || !proc_is_user_process(task->proc) ||
		    task_is_exiting(task)) {
			task_put(task);
			continue;
		}
		if (init_process_is_task(task) ||
		    (task->proc && task->proc->pid &&
		     task->proc->pid->nr == caller_tgid)) {
			task_put(task);
			continue;
		}

		found = true;
		if (!signal_may_send_to_task(task, sig)) {
			task_put(task);
			continue;
		}
		permitted = true;
		if (sig == 0) {
			delivered = true;
			task_put(task);
			continue;
		}
		ret = send_group_signal_info(sig, info, task);
		task_put(task);
		if (ret == 0)
			delivered = true;
		else if (ret < 0 && ret != -ESRCH && first_error == 0)
			first_error = ret;
	}

	if (delivered)
		return 0;
	if (!found)
		return -ESRCH;
	if (!permitted)
		return -EPERM;
	return first_error ? first_error : -ESRCH;
}

static bool signal_restartable(const struct trap_frame *tf, size_t nr)
{
	switch (nr) {
	case SYS_read:
	case SYS_write:
	case SYS_wait4:
		return true;
	case SYS_futex:
		return (syscall_arg(tf, 1) & FUTEX_CMD_MASK) == FUTEX_WAIT &&
		       syscall_arg(tf, 3) == 0;
	default:
		return false;
	}
}

static void signal_clear_frames(struct task_struct *task)
{
	struct signal_frame_state *state;

	if (!task)
		return;

	state = task->signal.signal_frames;
	while (state) {
		struct signal_frame_state *previous = state->previous;

		kfree(state);
		state = previous;
	}
	task->signal.signal_frames = NULL;
}

bool sig_valid(int sig)
{
	return sig > 0 && sig < SIGRTMIN;
}

static uint64_t signal_mask(int sig)
{
	return 1UL << (sig - 1);
}

bool sig_catchable(int sig)
{
	return sig != SIGKILL && sig != SIGSTOP;
}

static uint64_t unblockable_mask(void)
{
	return signal_mask(SIGKILL) | signal_mask(SIGSTOP);
}

struct sigchld_exit_policy sigchld_exit_policy(const struct proc_struct *proc)
{
	struct sigaction action = signal_sigchld_action(proc);
	struct sigchld_exit_policy policy;

	policy.auto_reap = action.sa_handler == SIG_IGN ||
			   (action.sa_flags & SA_NOCLDWAIT) != 0;
	policy.notify = action.sa_handler != SIG_IGN;
	return policy;
}

static bool signal_sigchld_stop_suppressed(const struct proc_struct *proc)
{
	struct sigaction action = signal_sigchld_action(proc);

	return action.sa_handler == SIG_IGN ||
	       (action.sa_flags & SA_NOCLDSTOP) != 0;
}

static bool signal_sigchld_ignored(const struct proc_struct *proc)
{
	return signal_sigchld_action(proc).sa_handler == SIG_IGN;
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
	struct sighand_struct *sighand;
	struct signal_struct *signal;

	if (!proc)
		return -EINVAL;

	sighand = sighand_alloc();
	if (!sighand)
		return -ENOMEM;
	proc->sighand = sighand;

	signal = signal_state_alloc();
	if (!signal) {
		sighand_put(sighand);
		proc->sighand = NULL;
		return -ENOMEM;
	}
	proc->signal = signal;
	return 0;
}

int sig_task_init(struct task_struct *task)
{
	int ret;

	if (!task || !task->proc)
		return -EINVAL;
	if (!task->proc->signal) {
		ret = sig_proc_init(task->proc);
		if (ret < 0)
			return ret;
	}

	sig_set_mask(task, 0);
	signal_clear_pending(task, ~0UL);
	task->signal.forced_pending = 0;
	atomic_set(&task->signal.has_pending_signal, 0);
	atomic_set(&task->signal.has_fatal_pending, 0);
	signal_clear_frames(task);
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
	signal = task->proc ? task->proc->signal : NULL;
	if (!signal)
		return false;
	spin_lock_irqsave(&signal->siglock, &flags);
	spin_lock(&task->wait.lock);
	if (task->lifecycle == TASK_LIVE) {
		task->lifecycle = TASK_EXITING;
		begun = true;
	}
	spin_unlock(&task->wait.lock);
	spin_unlock_irqrestore(&signal->siglock, flags);
	return begun;
}

void sig_proc_release(struct proc_struct *proc)
{
	if (!proc)
		return;

	sighand_put(proc->sighand);
	signal_state_put(proc->signal);
	proc->sighand = NULL;
	proc->signal = NULL;
}

void sig_task_release(struct task_struct *task)
{
	if (!task)
		return;

	sig_set_mask(task, 0);
	signal_clear_pending(task, ~0UL);
	task->signal.forced_pending = 0;
	signal_clear_frames(task);
	task->signal.restore_mask = 0;
	task->signal.restore_mask_pending = false;
	signal_reset_altstack(task);
}

int sig_task_clone(struct task_struct *child, bool share_sighand,
		   bool disable_altstack)
{
	struct sighand_struct *sighand;
	struct signal_struct *signal;

	if (!child)
		return -EINVAL;

	if (share_sighand) {
		sighand = current_task()->proc ? current_task()->proc->sighand
					       : NULL;
		if (!sighand)
			return -EINVAL;
		sighand_get(sighand);
	} else {
		sighand = sighand_dup(current_task()->proc
					      ? current_task()->proc->sighand
					      : NULL);
		if (!sighand)
			return -ENOMEM;
	}

	signal = signal_state_alloc();
	if (!signal) {
		sighand_put(sighand);
		return -ENOMEM;
	}

	sig_task_release(child);
	child->proc->sighand = sighand;
	child->proc->signal = signal;
	sig_set_mask(child, sig_blocked_mask(current_task()));
	signal_clear_pending(child, ~0UL);
	child->signal.forced_pending = 0;
	signal_clear_frames(child);
	child->signal.restore_mask = 0;
	child->signal.restore_mask_pending = false;
	{
		int ret = signal_frame_clone(child, current_task());

		if (ret < 0)
			return ret;
	}
	if (current_task() && !disable_altstack) {
		struct stack_t *child_sas = &child->signal.sas;

		*child_sas = current_task()->signal.sas;
		child_sas->ss_flags &= ~SS_ONSTACK;
	} else {
		signal_reset_altstack(child);
	}
	return 0;
}

int sig_exec_prepare(const struct task_struct *task,
		     struct sighand_struct **prepared)
{
	struct sighand_struct *sighand;

	if (!task || !prepared)
		return -EINVAL;
	*prepared = NULL;
	sighand = sighand_dup(task->proc ? task->proc->sighand : NULL);
	if (!sighand)
		return -ENOMEM;
	sighand_reset_for_exec(sighand);
	*prepared = sighand;
	return 0;
}

void sig_exec_commit(struct task_struct *task, struct sighand_struct *prepared)
{
	struct sighand_struct *old;

	BUG_ON(!task->proc);
	old = task->proc->sighand;
	task->proc->sighand = prepared;
	sighand_put(old);
	signal_clear_frames(task);
	signal_reset_altstack(task);
}

void sig_exec_abort(struct sighand_struct *prepared)
{
	sighand_put(prepared);
}

uint64_t sig_pending(const struct task_struct *task)
{
	uint64_t pending;
	struct signal_struct *signal;

	if (!task)
		return 0;

	signal = task->proc ? task->proc->signal : NULL;
	if (!signal)
		return task->signal.pending & task->signal.blocked;
	pending = task->signal.pending;
	if (signal) {
		irq_flags_t flags;

		spin_lock_irqsave(&signal->siglock, &flags);
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
	if (task->proc && task->proc->signal)
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
	signal = task->proc ? task->proc->signal : NULL;
	if (!signal)
		return task->signal.blocked;
	spin_lock_irqsave(&signal->siglock, &flags);
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
	signal = task->proc ? task->proc->signal : NULL;
	mask &= SIGNAL_STANDARD_MASK & ~unblockable_mask();
	if (!signal) {
		task->signal.blocked |= mask;
		return;
	}
	spin_lock_irqsave(&signal->siglock, &flags);
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
	signal = task->proc ? task->proc->signal : NULL;
	if (!signal) {
		task->signal.blocked &= ~mask;
		task->signal.blocked &= ~unblockable_mask();
		return;
	}
	spin_lock_irqsave(&signal->siglock, &flags);
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
	signal = task->proc ? task->proc->signal : NULL;
	if (!signal) {
		task->signal.blocked = mask;
		return;
	}
	spin_lock_irqsave(&signal->siglock, &flags);
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
	if (!task)
		return false;
	signal = task->proc ? task->proc->signal : NULL;
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
	signal = task->proc ? task->proc->signal : NULL;
	if (!signal) {
		signal_clear_pending_locked(task, mask);
		return;
	}
	spin_lock_irqsave(&signal->siglock, &flags);
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

static int send_group_signal_info(int sig, const siginfo_t *info,
				  struct task_struct *leader)
{
	struct task_struct **targets;
	struct signal_struct *signal;
	uint64_t mask;
	size_t count = 0;
	size_t index;
	irq_flags_t signal_flags;

	if (!sig_valid(sig))
		return -EINVAL;
	if (!info)
		return -EINVAL;
	/* Process-directed delivery must not bail on an exiting leader:
	 * shared pending plus live-sibling wake-up is still valid. Dead
	 * pids are rejected at the pid lookup layer. */

	signal = leader && leader->proc ? leader->proc->signal : NULL;
	if (!signal)
		return send_signal_info_internal(sig, info, leader, false);

	targets = kmalloc_array(PID_COUNT, sizeof(*targets), ALLOC_NOWAIT);
	if (!targets)
		return -ENOMEM;
	if (leader->proc)
		count = proc_task_snapshot(leader->proc, NULL, targets,
					   PID_COUNT);

	{
		struct sighand_struct *sighand = leader->proc->sighand;

		if (sighand)
			mutex_lock(&sighand->lock);

		spin_lock_irqsave(&signal->siglock, &signal_flags);
		mask = signal_mask(sig);
		if (signal_group_would_drop_at_raise_locked(leader, sig,
							    targets, count)) {
			spin_unlock_irqrestore(&signal->siglock, signal_flags);
			if (sighand)
				mutex_unlock(&sighand->lock);
			signal_task_snapshot_release(targets, count);
			return 0;
		}
		signal_clear_shared_opposite_pending(signal, sig);
		if (!(signal->shared_pending & mask)) {
			signal->shared_pending_info[sig] = *info;
			signal->shared_pending_info[sig].si_signo = sig;
			signal->shared_pending |= mask;
		}
		for (index = 0; index < count; index++) {
			signal_clear_opposite_pending_locked(targets[index],
							     sig);
		}
		signal_recalc_targets_locked(signal, leader->proc, targets,
					     count);
		spin_unlock_irqrestore(&signal->siglock, signal_flags);
		if (sighand)
			mutex_unlock(&sighand->lock);
	}

	if (leader->proc) {
		for (index = 0; index < count; index++) {
			wake_signal_target(targets[index], sig);
			task_put(targets[index]);
		}
		/* Resume a whole-group stop before asking proc to publish a
		 * possible aggregate continue transition.  The proc layer
		 * suppresses the event when no process-wide stop state was
		 * active. */
		if (sig == SIGCONT) {
			(void)signal_group_stop_clear(leader);
			struct proc_parent_event event;

			proc_publish_continue(leader->proc, &event);
			sig_notify_parent(&event);
			proc_parent_event_release(&event);
		}
	} else {
		wake_signal_target(leader, sig);
	}
	kfree(targets);

	return 0;
}

int sig_send_pgrp(int sig, const siginfo_t *info, struct pgrp_struct *pgrp,
		  struct session_struct *session)
{
	bool delivered = false;
	int first_error = 0;
	struct task_struct **tasks;
	size_t count;
	size_t index;

	if (sig != 0 && (!sig_valid(sig) || !info))
		return -EINVAL;
	if (!pgrp || !session || pgrp->session != session)
		return -ESRCH;
	tasks = kmalloc_array(PID_COUNT, sizeof(*tasks), ALLOC_NOWAIT);
	if (!tasks)
		return -ENOMEM;
	count = proc_pgrp_task_snapshot(pgrp, session, tasks, PID_COUNT);
	if (sig == 0) {
		signal_task_snapshot_release(tasks, count);
		return count ? 0 : -ESRCH;
	}
	for (index = 0; index < count; index++) {
		int ret = send_group_signal_info(sig, info, tasks[index]);

		if (ret == 0) {
			delivered = true;
		} else if (ret != -ESRCH && first_error == 0) {
			first_error = ret;
		}
		task_put(tasks[index]);
	}
	kfree(tasks);
	return delivered ? 0 : first_error ? first_error : -ESRCH;
}

void sig_orphan_pgrp(const struct proc_orphan_event *event)
{
	siginfo_t info = {0};

	if (!event || !event->pgrp || !event->session)
		return;
	info.si_code = SI_KERNEL;
	info.si_signo = SIGHUP;
	(void)sig_send_pgrp(SIGHUP, &info, event->pgrp, event->session);
	info.si_signo = SIGCONT;
	(void)sig_send_pgrp(SIGCONT, &info, event->pgrp, event->session);
}

void sig_notify_parent(const struct proc_parent_event *event)
{
	struct task_struct *target;
	siginfo_t info = {0};
	bool suppress;

	if (!event || !event->parent || !event->child)
		return;
	wait_channel_wake_all(&event->parent->wait_state.channel);
	if (event->signal == 0)
		return;
	if (event->code == CLD_EXITED || event->code == CLD_KILLED ||
	    event->code == CLD_DUMPED) {
		/* Exit notification is frozen at child exit.  Auto-reap is
		 * independent: SA_NOCLDWAIT still reports SIGCHLD when the
		 * disposition is not SIG_IGN. */
		suppress = !event->child->exit_sigchld_notify;
	} else {
		suppress = signal_sigchld_ignored(event->parent) ||
			   signal_sigchld_stop_suppressed(event->parent);
	}
	if (suppress)
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

	if (signal_frame_contains(task, sig)) {
		if (task == current_task())
			do_exit_signal(sig);
		return 0;
	}

	signal_unblock_mask(task, signal_mask(sig));
	{
		struct sighand_struct *sighand =
			task->proc ? task->proc->sighand : NULL;

		if (sighand) {
			mutex_lock(&sighand->lock);
			if (sighand->sigactions[sig].sa_handler == SIG_IGN)
				sighand->sigactions[sig].sa_handler = SIG_DFL;
			mutex_unlock(&sighand->lock);
		}
	}

	ret = send_signal_info_internal(sig, info, task, true);
	if (ret < 0)
		return ret;

	return 0;
}

void sig_init(void)
{
	int ret;

	ret = user_map_register_reserved(
		"signal_trampoline", SIGNAL_TRAMPOLINE_ADDR,
		SIGNAL_TRAMPOLINE_ADDR + PAGE_SIZE, signal_map_trampoline);
	BUG_ON(ret < 0);
}

/* True when the current task's thread group is stopped; used at the user
 * return boundary so a blocked member that could not be stopped synchronously
 * stops itself on its next return to user mode. */
static bool signal_group_stopped(const struct task_struct *task)
{
	struct signal_struct *signal =
		task && task->proc ? task->proc->signal : NULL;
	irq_flags_t flags;
	bool stopped;

	if (!signal)
		return false;
	spin_lock_irqsave(&signal->siglock, &flags);
	stopped = signal->group_stopped;
	spin_unlock_irqrestore(&signal->siglock, flags);
	return stopped;
}

int sig_suspend(uint64_t mask)
{
	struct wait_deadline deadline = wait_deadline_none();
	struct task_wait *wait = &current_task()->wait;
	uint64_t blocked;
	wait_outcome_t outcome;
	int ret;

	if (mask & ~SIGNAL_STANDARD_MASK)
		return -EINVAL;

	blocked = sig_blocked_mask(current_task());
	sig_set_mask(current_task(), mask);
	ret = wait_start(wait, WAIT_FLAG_INTERRUPTIBLE, &deadline);
	if (ret == 0)
		ret = wait_block(wait, &outcome);
	wait_finish(wait);
	if (ret < 0) {
		sig_set_mask(current_task(), blocked);
		return ret;
	}

	BUG_ON(outcome != WAIT_OUTCOME_SIGNAL);
	sig_defer_mask_restore(current_task(), blocked);
	return -EINTR;
}

int sig_wait(uint64_t set, const struct timespec *timeout, siginfo_t *info)
{
	struct wait_deadline deadline;
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
	ret = signal_wait_start_set(current_task(), &deadline, set);
	if (ret == 0) {
		ret = wait_block(&current_task()->wait, &outcome);
		wait_finish(&current_task()->wait);
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

		/* A member of a group that is stopped (by a sibling's stop
		 * default action) stops itself here rather than returning to
		 * user, so the whole thread group is stopped, not just the
		 * delivering thread. A fatal (SIGKILL) breaks the stop: it must
		 * reach delivery and terminate the task instead of re-stopping.
		 */
		if (signal_group_stopped(current_task()) &&
		    !sig_fatal_pending(current_task())) {
			(void)sched_stop(current_task());
			if (irqs_disabled())
				schedule_irqoff();
			else
				schedule();
			continue;
		}

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
			current_task()->proc ? current_task()->proc->signal
					     : NULL;
		struct task_struct **targets = NULL;
		size_t target_count = 0;
		irq_flags_t flags;

		if (shared && signal && current_task()->proc) {
			targets = kmalloc_array(PID_COUNT, sizeof(*targets),
						ALLOC_NOWAIT);
			if (targets)
				target_count = proc_task_snapshot(
					current_task()->proc, NULL, targets,
					PID_COUNT);
		}

		/* Per-task and shared pending, plus the forced attribute, are
		 * consumed under one siglock acquisition so the action and the
		 * pending store stay consistent with the lock-free
		 * next_signal() hint. */
		if (signal)
			spin_lock_irqsave(&signal->siglock, &flags);
		if (shared) {
			forced = false;
			if (!(signal && (signal->shared_pending & mask))) {
				if (signal)
					spin_unlock_irqrestore(&signal->siglock,
							       flags);
				signal_task_snapshot_release(targets,
							     target_count);
				continue;
			}
			info = signal->shared_pending_info[sig];
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
			info = current_task()->signal.pending_info[sig];
			forced = (current_task()->signal.forced_pending &
				  mask) != 0;
			signal_clear_pending_locked(current_task(), mask);
		}
		if (signal) {
			if (shared)
				signal_recalc_targets_locked(
					signal, current_task()->proc, targets,
					target_count);
			signal_recalc_facts_locked(current_task(), signal);
		}
		if (signal)
			spin_unlock_irqrestore(&signal->siglock, flags);
		signal_task_snapshot_release(targets, target_count);

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
			case SIGNAL_DEFAULT_CONTINUE:
				continue;
			case SIGNAL_DEFAULT_STOP:
				stop_current(sig, shared);
				continue;
			case SIGNAL_DEFAULT_TERMINATE:
				do_exit_signal(sig);
				continue;
			}
		}

		if (action.sa_flags & SA_RESETHAND)
			reset_signal_action(sig);

		if (rseq_signal_deliver(tf) < 0)
			do_exit_signal(SIGSEGV);
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
	long pgid;
	pid_t current_pgid;
	pid_t sid;
	int ret;

	if (sig != 0 && !sig_valid(sig))
		return -EINVAL;

	signal_init_user_info(&info, sig);
	if (pid == -1)
		return kill_all_processes(sig, &info);
	if (pid == 0) {
		ret = proc_snapshot_topology(current_task()->proc,
					     &current_pgid, &sid);
		if (ret < 0)
			return ret;
		return send_pgrp_signal_info(sig, &info, current_pgid, 0);
	}
	if (pid < -1) {
		pgid = -(long)pid;
		if (pgid > PID_MAX)
			return -ESRCH;
		return send_pgrp_signal_info(sig, &info, (pid_t)pgid, 0);
	}

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

int sig_tkill(pid_t tid, int sig)
{
	struct task_struct *task;
	siginfo_t info;

	if (sig != 0 && !sig_valid(sig))
		return -EINVAL;
	if (tid <= 0)
		return -EINVAL;

	task = pid_lookup_task(tid);
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

	signal_init_user_info(&info, sig);
	int ret = send_signal_info_internal(sig, &info, task, false);

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
		SA_SIGINFO | SA_ONSTACK | SA_RESTART | SA_NODEFER |
		SA_RESETHAND | SA_NOCLDSTOP | SA_NOCLDWAIT;
	struct signal_struct *signal =
		current_task()->proc ? current_task()->proc->signal : NULL;
	struct sighand_struct *sighand =
		current_task()->proc ? current_task()->proc->sighand : NULL;
	struct task_struct **targets = NULL;
	struct sigaction kact;
	size_t target_count = 0;
	bool discard_pending = false;
	irq_flags_t flags;

	if (!sig_valid(sig))
		return -EINVAL;
	if (!sig_catchable(sig) && act)
		return -EINVAL;
	if (!sighand || (act && !signal))
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
	if (discard_pending) {
		targets = kmalloc_array(PID_COUNT, sizeof(*targets),
					ALLOC_NOWAIT);
		if (!targets)
			return -ENOMEM;
		target_count = proc_task_snapshot(current_task()->proc, NULL,
						  targets, PID_COUNT);
	}

	mutex_lock(&sighand->lock);
	if (oldact)
		*oldact = sighand->sigactions[sig];
	if (act) {
		if (discard_pending) {
			spin_lock_irqsave(&signal->siglock, &flags);
			signal_discard_ignored_pending_locked(
				signal, targets, target_count, sig);
			spin_unlock_irqrestore(&signal->siglock, flags);
		}
		sighand->sigactions[sig] = kact;
	}
	mutex_unlock(&sighand->lock);
	signal_task_snapshot_release(targets, target_count);
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

ssize_t sig_return(struct trap_frame *tf, uintptr_t sp)
{
	struct task_struct *task = current_task();
	struct rt_sigframe frame;
	struct rt_sigframe *user_frame = (struct rt_sigframe *)sp;
	const unsigned char *fp_state;
	struct signal_frame_state *state = current_task()->signal.signal_frames;

	if (!state || state->sp != sp)
		do_exit_signal(SIGSEGV);

	if (copy_from_user(&frame, user_frame, sizeof(frame)) != 0)
		do_exit_signal(SIGSEGV);
	if (frame.info.si_signo != state->sig)
		do_exit_signal(SIGSEGV);
	if (frame.uc.uc_flags != 0 || frame.uc.uc_link != NULL)
		do_exit_signal(SIGSEGV);
	if ((frame.uc.uc_mcontext.sc_regs.pc & 1) ||
	    !access_ok((const void *)frame.uc.uc_mcontext.sc_regs.pc, 1))
		do_exit_signal(SIGSEGV);
	if ((frame.uc.uc_mcontext.sc_regs.sp & 0xf) ||
	    frame.uc.uc_mcontext.sc_regs.sp == 0 ||
	    !access_ok((const void *)(frame.uc.uc_mcontext.sc_regs.sp - 1), 1))
		do_exit_signal(SIGSEGV);
	if (frame.uc.uc_stack.ss_flags != 0 &&
	    frame.uc.uc_stack.ss_flags != SS_DISABLE)
		do_exit_signal(SIGSEGV);
	if (!(frame.uc.uc_stack.ss_flags & SS_DISABLE) &&
	    (frame.uc.uc_stack.ss_size < MINSIGSTKSZ ||
	     !access_ok(frame.uc.uc_stack.ss_sp, frame.uc.uc_stack.ss_size)))
		do_exit_signal(SIGSEGV);
	/* F/D/vector state is unsupported, so extension state remains zero. */
	fp_state = (const unsigned char *)&frame.uc.uc_mcontext.sc_fpregs;
	for (size_t index = 0; index < sizeof(frame.uc.uc_mcontext.sc_fpregs);
	     index++) {
		if (fp_state[index] != 0)
			do_exit_signal(SIGSEGV);
	}

	signal_restore_user_regs(tf, &frame.uc.uc_mcontext.sc_regs);
	sig_set_mask(current_task(), frame.uc.uc_sigmask);
	signal_frame_pop(current_task());
	current_task()->signal.sas = frame.uc.uc_stack;
	task->arch.tf = tf;
	return (ssize_t)trap_return_value(tf);
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
		context->args[index] = syscall_arg(tf, index);
	context->nr = nr;
	context->valid = true;
	context->restartable = signal_restartable(tf, nr);
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
