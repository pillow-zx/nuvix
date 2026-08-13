/*
 * kernel/signal.c - 信号机制
 */

#include <kernel/errno.h>
#include <kernel/exit.h>
#include <kernel/buddy.h>
#include <kernel/compiler.h>
#include <kernel/fs.h>
#include <kernel/init.h>
#include <kernel/mm.h>
#include <kernel/pid.h>
#include <kernel/printk.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/rseq.h>
#include <kernel/signal.h>
#include <kernel/slab.h>
#include <kernel/task.h>
#include <kernel/syscall.h>
#include <kernel/time.h>
#include <kernel/user_map.h>
#include <kernel/wait.h>
#include <uapi/futex.h>
#include <uapi/syscall.h>
#include <kernel/processor.h>
#include <kernel/page.h>
#include <kernel/pgtable.h>
#include <kernel/trap.h>

static void *trampoline_page;

struct signal_frame_state {
	uintptr_t sp;
	int sig;
	struct signal_frame_state *previous;
};

static_assert(SYS_rt_sigreturn >= 0 && SYS_rt_sigreturn < 2048,
	      "SYS_sigreturn must fit in a RISC-V addi immediate");

#define RISCV_REG_ZERO 0
#define RISCV_REG_A7   17
#define RISCV_OP_IMM   0x13
#define RISCV_ECALL    0x00000073
#define RISCV_J_SELF   0x0000006f

#define RISCV_ADDI(rd, rs1, imm)                                               \
	((((uint32_t)(imm) & 0xfff) << 20) | ((uint32_t)(rs1) << 15) |         \
	 ((uint32_t)(rd) << 7) | RISCV_OP_IMM)

static struct signal_frame_state *
signal_frame_top(const struct task_struct *task)
{
	return task ? task->signal.signal_frames : NULL;
}

void signal_clear_frames(struct task_struct *task)
{
	struct signal_frame_state *state;

	if (!task)
		return;

	state = signal_frame_top(task);
	while (state) {
		struct signal_frame_state *previous = state->previous;

		kfree(state);
		state = previous;
	}
	task->signal.signal_frames = NULL;
}

static int signal_frame_clone(struct task_struct *child,
			      const struct task_struct *parent)
{
	const struct signal_frame_state *source;
	struct signal_frame_state **destination = &child->signal.signal_frames;

	for (source = signal_frame_top(parent); source;
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
	state->previous = signal_frame_top(task);
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
	struct signal_frame_state *state = signal_frame_top(task);

	BUG_ON(!state);
	task->signal.signal_frames = state->previous;
	kfree(state);
}

static bool signal_frame_contains(const struct task_struct *task, int sig)
{
	const struct signal_frame_state *state = signal_frame_top(task);

	for (; state; state = state->previous) {
		if (state->sig == sig)
			return true;
	}
	return false;
}

bool signal_is_valid(int sig)
{
	return sig > 0 && sig <= NSIG;
}

uint64_t signal_mask(int sig)
{
	return 1UL << (sig - 1);
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

static bool signal_is_fatal_default(int sig)
{
	switch (sig) {
	case SIGCHLD:
	case SIGCONT:
	case SIGTSTP:
	case SIGTTIN:
	case SIGTTOU:
		return false;
	default:
		return true;
	}
}

static bool signal_stops_default(int sig, __sighandler_t handler)
{
	if (sig == SIGSTOP)
		return true;
	if (handler != SIG_DFL)
		return false;

	return signal_is_stop_signal(sig);
}

bool signal_is_catchable(int sig)
{
	return sig != SIGKILL && sig != SIGSTOP;
}

uint64_t unblockable_mask(void)
{
	return signal_mask(SIGKILL) | signal_mask(SIGSTOP);
}

static struct sighand_struct *sighand_alloc(void)
{
	struct sighand_struct *sighand =
		kmalloc(sizeof(*sighand), ALLOC_NOWAIT);

	if (!sighand)
		return NULL;

	memset(sighand, 0, sizeof(*sighand));
	refcount_set(&sighand->refcount, 1);
	mutex_init(&sighand->lock, LOCK_RANK_SIGNAL);
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
	mutex_init(&signal->lock, LOCK_RANK_SIGNAL);
	return signal;
}

static void signal_state_put(struct signal_struct *signal)
{
	if (!signal)
		return;

	if (refcount_dec_and_test(&signal->refcount))
		kfree(signal);
}

static bool task_signal_target_dead(struct task_struct *task)
{
	return !task || task_is_exiting(task);
}

static __sighandler_t signal_handler_for_task(struct task_struct *task, int sig)
{
	struct sighand_struct *sighand = task_sighand(task);
	__sighandler_t handler = SIG_DFL;

	if (!sighand)
		return handler;

	mutex_lock(&sighand->lock);
	handler = sighand->sigactions[sig].sa_handler;
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

struct sigchld_exit_policy signal_sigchld_exit_policy(
	const struct proc_struct *proc)
{
	struct sigaction action = signal_sigchld_action(proc);
	struct sigchld_exit_policy policy;

	policy.auto_reap = action.sa_handler == SIG_IGN ||
			   (action.sa_flags & SA_NOCLDWAIT) != 0;
	policy.notify = action.sa_handler != SIG_IGN;
	return policy;
}

bool signal_sigchld_stop_suppressed(const struct proc_struct *proc)
{
	struct sigaction action = signal_sigchld_action(proc);

	return action.sa_handler == SIG_IGN ||
	       (action.sa_flags & SA_NOCLDSTOP) != 0;
}

bool signal_sigchld_ignored(const struct proc_struct *proc)
{
	return signal_sigchld_action(proc).sa_handler == SIG_IGN;
}

static bool signal_init_default_ignored(struct task_struct *task, int sig)
{
	if (!init_process_is_task(task) || sig == SIGSTOP)
		return false;

	return (signal_is_fatal_default(sig) || sig == SIGTSTP) &&
	       signal_handler_for_task(task, sig) == SIG_DFL;
}

void signal_reset_altstack(struct task_struct *task)
{
	struct stack_t *sas = task_altstack_safe(task);

	if (!sas)
		return;
	sas->ss_sp = NULL;
	sas->ss_flags = SS_DISABLE;
	sas->ss_size = 0;
}

int signal_proc_init(struct proc_struct *proc)
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

int signals_init(struct task_struct *task)
{
	int ret;

	if (!task || !task->proc)
		return -EINVAL;
	if (!task->proc->signal) {
		ret = signal_proc_init(task->proc);
		if (ret < 0)
			return ret;
	}

	signal_set_blocked_mask(task, 0);
	signal_clear_pending(task, ~0UL);
	task->signal.forced_pending = 0;
	signal_clear_frames(task);
	task->signal.restore_mask = 0;
	task->signal.restore_mask_pending = false;
	signal_reset_altstack(task);
	return 0;
}

void signal_write_child_tid(struct task_struct *task)
{
	int *child_tid;
	int tid;

	if (!task)
		return;
	child_tid = task->signal.set_child_tid;
	if (!child_tid)
		return;
	tid = task->tid ? task->tid->nr : 0;
	if (copy_to_user(child_tid, &tid, sizeof(tid)) != 0)
		pr_debug("signal: CLONE_CHILD_SETTID write failed for tid=%d\n",
			 tid);
	task->signal.set_child_tid = NULL;
}

void signal_proc_release(struct proc_struct *proc)
{
	if (!proc)
		return;

	sighand_put(proc->sighand);
	signal_state_put(proc->signal);
	proc->sighand = NULL;
	proc->signal = NULL;
}

void signals_release(struct task_struct *task)
{
	if (!task)
		return;

	signal_set_blocked_mask(task, 0);
	signal_clear_pending(task, ~0UL);
	task->signal.forced_pending = 0;
	signal_clear_frames(task);
	task->signal.restore_mask = 0;
	task->signal.restore_mask_pending = false;
	signal_reset_altstack(task);
}

int signals_clone(struct task_struct *child, bool share_sighand,
		  bool disable_altstack)
{
	struct sighand_struct *sighand;
	struct signal_struct *signal;

	if (!child)
		return -EINVAL;

	if (share_sighand) {
		sighand = task_sighand(current_task());
		if (!sighand)
			return -EINVAL;
		sighand_get(sighand);
	} else {
		sighand = sighand_dup(task_sighand(current_task()));
		if (!sighand)
			return -ENOMEM;
	}

	signal = signal_state_alloc();
	if (!signal) {
		sighand_put(sighand);
		return -ENOMEM;
	}

	signals_release(child);
	child->proc->sighand = sighand;
	child->proc->signal = signal;
	signal_set_blocked_mask(child, signal_blocked_mask(current_task()));
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
		struct stack_t *child_sas = task_altstack(child);

		*child_sas = *task_altstack(current_task());
		child_sas->ss_flags &= ~SS_ONSTACK;
	} else {
		signal_reset_altstack(child);
	}
	return 0;
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

int signals_prepare_exec(const struct task_struct *task,
			 struct sighand_struct **prepared)
{
	struct sighand_struct *sighand;

	if (!task || !prepared)
		return -EINVAL;
	*prepared = NULL;
	sighand = sighand_dup(task_sighand(task));
	if (!sighand)
		return -ENOMEM;
	sighand_reset_for_exec(sighand);
	*prepared = sighand;
	return 0;
}

void signals_commit_exec(struct task_struct *task,
			 struct sighand_struct *prepared)
{
	struct sighand_struct *old;

	BUG_ON(!task || !task->proc || !prepared);
	old = task_sighand(task);
	task->proc->sighand = prepared;
	sighand_put(old);
}

void signals_abort_exec(struct sighand_struct *prepared)
{
	sighand_put(prepared);
}

bool signal_pending(struct task_struct *task)
{
	uint64_t blocked;

	if (!task)
		return false;

	blocked = task_blocked_mask(task) & ~unblockable_mask();
	return (signal_pending_mask(task) & ~blocked) != 0;
}

uint64_t signal_pending_mask(const struct task_struct *task)
{
	uint64_t pending;
	struct signal_struct *signal;

	if (!task)
		return 0;

	pending = task_pending_mask(task);
	signal = task_signal_state(task);
	if (signal) {
		mutex_lock(&signal->lock);
		pending |= signal->shared_pending;
		mutex_unlock(&signal->lock);
	}
	return pending;
}

bool signal_fatal_pending(struct task_struct *task)
{
	uint64_t pending;
	uint64_t forced;
	uint64_t kill_mask = signal_mask(SIGKILL);

	if (!task)
		return false;

	pending = task_pending_mask(task);
	forced = task->signal.forced_pending;
	if (task_signal_state(task)) {
		struct signal_struct *signal = task_signal_state(task);

		mutex_lock(&signal->lock);
		pending |= signal->shared_pending;
		mutex_unlock(&signal->lock);
	}
	return (pending & kill_mask) != 0 &&
	       ((forced & kill_mask) != 0 ||
		!signal_init_default_ignored(task, SIGKILL));
}

uint64_t signal_blocked_mask(struct task_struct *task)
{
	return task_blocked_mask(task);
}

void signal_block_mask(struct task_struct *task, uint64_t mask)
{
	task_or_blocked_mask(task, mask);
	task_and_blocked_mask(task, ~unblockable_mask());
}

void signal_unblock_mask(struct task_struct *task, uint64_t mask)
{
	task_and_blocked_mask(task, ~mask);
	task_and_blocked_mask(task, ~unblockable_mask());
}

void signal_set_blocked_mask(struct task_struct *task, uint64_t mask)
{
	task_set_blocked_mask(task, mask & ~unblockable_mask());
}

void signal_mark_pending(struct task_struct *task, uint64_t mask)
{
	task_or_pending_mask(task, mask);
}

void signal_clear_pending(struct task_struct *task, uint64_t mask)
{
	if (!task)
		return;

	for (int sig = 1; sig <= NSIG; sig++) {
		if (mask & signal_mask(sig))
			memset(&task->signal.pending_info[sig], 0,
			       sizeof(task->signal.pending_info[sig]));
	}
	task_and_pending_mask(task, ~mask);
	task->signal.forced_pending &= ~mask;
}

static void signal_clear_opposite_pending(struct task_struct *task, int sig)
{
	if (sig == SIGCONT)
		signal_clear_pending(task, signal_stop_mask());
	else if (signal_is_stop_signal(sig))
		signal_clear_pending(task, signal_mask(SIGCONT));
}

static void signal_clear_shared_opposite_pending(struct signal_struct *signal,
						 int sig)
{
	uint64_t mask = 0;

	if (sig == SIGCONT)
		mask = signal_stop_mask();
	else if (signal_is_stop_signal(sig))
		mask = signal_mask(SIGCONT);

	signal->shared_pending &= ~mask;
}

static void signal_clear_task_shared_opposite_pending(struct task_struct *task,
						      int sig)
{
	struct signal_struct *signal = task_signal_state(task);

	if (!signal)
		return;

	mutex_lock(&signal->lock);
	signal_clear_shared_opposite_pending(signal, sig);
	mutex_unlock(&signal->lock);
}

void signal_defer_mask_restore(struct task_struct *task, uint64_t mask)
{
	if (!task)
		return;

	task->signal.restore_mask = mask & ~unblockable_mask();
	task->signal.restore_mask_pending = true;
}

static bool signal_restore_mask_pending(struct task_struct *task)
{
	return task && task->signal.restore_mask_pending;
}

static uint64_t signal_take_restore_mask(struct task_struct *task)
{
	uint64_t mask = task->signal.restore_mask;

	task->signal.restore_mask = 0;
	task->signal.restore_mask_pending = false;
	return mask;
}

static void signal_restore_deferred_mask(struct task_struct *task)
{
	if (signal_restore_mask_pending(task))
		signal_set_blocked_mask(task, signal_take_restore_mask(task));
}

static void wake_signal_target(struct task_struct *task, int sig)
{
	if (!task)
		return;

	if (task->run_state == TASK_BLOCKED &&
	    ((signal_mask(sig) & ~task_blocked_mask(task)) ||
	     !signal_is_catchable(sig))) {
		(void)wait_wake_signal(task, false);
		return;
	}
	if (task->run_state == TASK_BLOCKED && signal_fatal_pending(task)) {
		(void)wait_wake_signal(task, true);
		return;
	}

	if (sig == SIGCONT && task->run_state == TASK_STOPPED) {
		(void)sched_resume(task);
		return;
	}

	if (sig == SIGKILL) {
		if (task->run_state == TASK_STOPPED)
			(void)sched_resume(task);
		else if (task->run_state == TASK_BLOCKED)
			(void)wait_wake_signal(task, true);
	}
}

static int send_signal_info_internal(int sig, const siginfo_t *info,
				     struct task_struct *task, bool force)
{
	uint64_t mask;

	if (!signal_is_valid(sig))
		return -EINVAL;
	if (!info)
		return -EINVAL;
	if (task_signal_target_dead(task))
		return -ESRCH;

	mask = signal_mask(sig);
	signal_clear_opposite_pending(task, sig);
	signal_clear_task_shared_opposite_pending(task, sig);
	if (!(task_pending_mask(task) & mask)) {
		task->signal.pending_info[sig] = *info;
		task->signal.pending_info[sig].si_signo = sig;
		signal_mark_pending(task, mask);
	}
	if (force)
		task->signal.forced_pending |= mask;
	wake_signal_target(task, sig);
	if (sig == SIGCONT && task->proc) {
		struct proc_parent_event event;

		proc_publish_continue(task->proc, &event);
		signal_notify_proc_parent(&event);
		proc_parent_event_release(&event);
	}

	return 0;
}

int send_signal_info(int sig, const siginfo_t *info, struct task_struct *task)
{
	return send_signal_info_internal(sig, info, task, false);
}

int send_signal(int sig, struct task_struct *task)
{
	siginfo_t info = {0};

	info.si_signo = sig;
	info.si_code = SI_KERNEL;
	return send_signal_info(sig, &info, task);
}

int send_current_signal(int sig)
{
	return send_signal(sig, current_task());
}

int send_group_signal_info(int sig, const siginfo_t *info,
			   struct task_struct *leader)
{
	struct task_struct **targets;
	uint64_t mask;
	size_t count;

	if (!signal_is_valid(sig))
		return -EINVAL;
	if (!info)
		return -EINVAL;
	/* Process-directed delivery must not bail on an exiting leader:
	 * shared pending plus live-sibling wake-up is still valid. Dead
	 * pids are rejected at the pid lookup layer. */

	if (!task_signal_state(leader))
		return send_signal_info(sig, info, leader);

	struct signal_struct *signal = task_signal_state(leader);

	targets = kmalloc_array(PID_COUNT, sizeof(*targets), ALLOC_NOWAIT);
	if (!targets)
		return -ENOMEM;

	mutex_lock(&signal->lock);
	mask = signal_mask(sig);
	signal_clear_shared_opposite_pending(signal, sig);
	if (!(signal->shared_pending & mask)) {
		signal->shared_pending_info[sig] = *info;
		signal->shared_pending_info[sig].si_signo = sig;
		signal->shared_pending |= mask;
	}
	mutex_unlock(&signal->lock);

	signal_clear_opposite_pending(leader, sig);
	if (leader->proc) {
		count = proc_task_snapshot(leader->proc, NULL, targets,
					   PID_COUNT);
		for (size_t index = 0; index < count; index++) {
			signal_clear_opposite_pending(targets[index], sig);
			wake_signal_target(targets[index], sig);
			task_put(targets[index]);
		}
		/* Publish once per proc, after all targets were woken, so the
		 * continue event reflects the whole-proc resume state. */
		if (sig == SIGCONT) {
			struct proc_parent_event event;

			proc_publish_continue(leader->proc, &event);
			signal_notify_proc_parent(&event);
			proc_parent_event_release(&event);
		}
	} else {
		wake_signal_target(leader, sig);
	}
	kfree(targets);

	return 0;
}

int send_group_signal(int sig, struct task_struct *leader)
{
	siginfo_t info = {0};

	info.si_signo = sig;
	info.si_code = SI_KERNEL;
	return send_group_signal_info(sig, &info, leader);
}

int signal_pending_info(const struct task_struct *task, int sig,
			siginfo_t *info)
{
	if (!task || !info || !signal_is_valid(sig))
		return -EINVAL;
	if (!(task_pending_mask(task) & signal_mask(sig)))
		return -ENOENT;

	*info = task->signal.pending_info[sig];
	return 0;
}

int signal_send_pgrp_snapshot(int sig, const siginfo_t *info,
			      struct pgrp_struct *pgrp,
			      struct session_struct *session)
{
	bool delivered = false;
	int first_error = 0;
	struct task_struct **tasks;
	size_t count;
	size_t index;

	if (sig != 0 && (!signal_is_valid(sig) || !info))
		return -EINVAL;
	if (!pgrp || !session || pgrp->session != session)
		return -ESRCH;
	tasks = kmalloc_array(PID_COUNT, sizeof(*tasks), ALLOC_NOWAIT);
	if (!tasks)
		return -ENOMEM;
	count = proc_pgrp_task_snapshot(pgrp, session, tasks, PID_COUNT);
	if (sig == 0) {
		for (index = 0; index < count; index++)
			task_put(tasks[index]);
		kfree(tasks);
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
	ret = signal_send_pgrp_snapshot(sig, info, pgrp, session);
	proc_session_put(session);
	proc_pgrp_put(pgrp);
	return ret;
}

void signal_orphaned_pgrp_event(const struct proc_orphan_event *event)
{
	siginfo_t info = {0};

	if (!event || !event->pgrp || !event->session)
		return;
	info.si_code = SI_KERNEL;
	info.si_signo = SIGHUP;
	(void)signal_send_pgrp_snapshot(SIGHUP, &info, event->pgrp,
					event->session);
	info.si_signo = SIGCONT;
	(void)signal_send_pgrp_snapshot(SIGCONT, &info, event->pgrp,
					event->session);
}

void signal_notify_proc_parent(const struct proc_parent_event *event)
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

int force_signal_info(int sig, const siginfo_t *info, struct task_struct *task)
{
	int ret;

	if (!signal_is_valid(sig))
		return -EINVAL;
	if (!task)
		return -ESRCH;

	if (signal_frame_contains(task, sig)) {
		if (task == current_task())
			do_exit_signal(sig);
		return 0;
	}

	signal_unblock_mask(task, signal_mask(sig));
	if (task_sighand(task)) {
		struct sighand_struct *sighand = task_sighand(task);

		mutex_lock(&sighand->lock);
		if (sighand->sigactions[sig].sa_handler == SIG_IGN)
			sighand->sigactions[sig].sa_handler = SIG_DFL;
		mutex_unlock(&sighand->lock);
	}

	ret = send_signal_info_internal(sig, info, task, true);
	if (ret < 0)
		return ret;

	return 0;
}

int force_signal(int sig, struct task_struct *task)
{
	siginfo_t info = {0};

	info.si_signo = sig;
	info.si_code = SI_KERNEL;
	return force_signal_info(sig, &info, task);
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

void signal_user_map_init(void)
{
	int ret;

	ret = user_map_register_reserved(
		"signal_trampoline", SIGNAL_TRAMPOLINE_ADDR,
		SIGNAL_TRAMPOLINE_ADDR + PAGE_SIZE, signal_map_trampoline);
	BUG_ON(ret < 0);
}

static void stop_current(int sig)
{
	struct proc_parent_event event;

	(void)sched_stop(current_task());
	if (current_task()->proc) {
		proc_publish_stop(current_task()->proc, sig, &event);
		signal_notify_proc_parent(&event);
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
	struct stack_t *sas = task_altstack(current_task());
	bool on_altstack = false;

	memset(&frame, 0, sizeof(frame));

	if ((action->sa_flags & SA_ONSTACK) && sas &&
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
	if (sas)
		frame.uc.uc_stack = *sas;
	signal_save_user_regs(&frame.uc.uc_mcontext.sc_regs, tf);
	if (signal_restore_mask_pending(current_task()))
		frame.uc.uc_sigmask = signal_take_restore_mask(current_task());
	else
		frame.uc.uc_sigmask = signal_blocked_mask(current_task());

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
	struct signal_struct *signal = task_signal_state(current_task());
	uint64_t pending = 0;

	if (!signal)
		return 0;

	mutex_lock(&signal->lock);
	pending = signal->shared_pending;
	mutex_unlock(&signal->lock);
	return pending;
}

static int take_shared_pending_from_set(uint64_t set, siginfo_t *info)
{
	struct signal_struct *signal = task_signal_state(current_task());
	int selected = 0;

	if (!signal || !info)
		return 0;

	mutex_lock(&signal->lock);
	for (int sig = 1; sig <= NSIG; sig++) {
		uint64_t mask = signal_mask(sig);

		if (!(signal->shared_pending & set & mask))
			continue;
		*info = signal->shared_pending_info[sig];
		signal->shared_pending &= ~mask;
		memset(&signal->shared_pending_info[sig], 0,
		       sizeof(signal->shared_pending_info[sig]));
		selected = sig;
		break;
	}
	mutex_unlock(&signal->lock);
	return selected;
}

static int take_shared_pending(int sig, siginfo_t *info)
{
	return take_shared_pending_from_set(signal_mask(sig), info) == sig
		       ? 0
		       : -ENOENT;
}

static int take_pending_from_set(uint64_t set, siginfo_t *info)
{
	uint64_t pending = task_pending_mask(current_task()) & set;

	for (int sig = 1; sig <= NSIG; sig++) {
		uint64_t mask = signal_mask(sig);

		if (!(pending & mask))
			continue;
		if (signal_pending_info(current_task(), sig, info) < 0)
			continue;
		signal_clear_pending(current_task(), mask);
		return sig;
	}

	return take_shared_pending_from_set(set, info);
}

int signal_sigsuspend(uint64_t mask)
{
	struct wait_deadline deadline = wait_deadline_none();
	struct task_wait *wait = &current_task()->wait;
	uint64_t blocked;
	wait_outcome_t outcome;
	int ret;

	blocked = signal_blocked_mask(current_task());
	signal_set_blocked_mask(current_task(), mask);
	ret = wait_start(wait, WAIT_FLAG_INTERRUPTIBLE, &deadline);
	if (ret == 0)
		ret = wait_block(wait, &outcome);
	wait_finish(wait);
	if (ret < 0) {
		signal_set_blocked_mask(current_task(), blocked);
		return ret;
	}

	BUG_ON(outcome != WAIT_OUTCOME_SIGNAL);
	signal_defer_mask_restore(current_task(), blocked);
	return -EINTR;
}

int signal_wait_pending(uint64_t set, const struct timespec *timeout,
			siginfo_t *info)
{
	struct wait_deadline deadline;
	uint64_t blocked;
	wait_outcome_t outcome;
	int sig;
	int ret;

	ret = mtime_deadline_from_timespec(timeout, &deadline);
	if (ret < 0)
		return ret;

	set &= ~unblockable_mask();
	sig = take_pending_from_set(set, info);
	if (sig)
		return sig;
	if (timeout && timeout->tv_sec == 0 && timeout->tv_nsec == 0)
		return -EAGAIN;

	blocked = signal_blocked_mask(current_task());
	signal_set_blocked_mask(current_task(), blocked & ~set);
	ret = wait_start(&current_task()->wait, WAIT_FLAG_INTERRUPTIBLE,
			 &deadline);
	if (ret == 0)
		ret = wait_block(&current_task()->wait, &outcome);
	wait_finish(&current_task()->wait);
	signal_set_blocked_mask(current_task(), blocked);
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

static int next_signal(bool *shared)
{
	uint64_t shared_pending = current_shared_pending();
	uint64_t pending = task_pending_mask(current_task()) | shared_pending;
	uint64_t deliverable;

	*shared = false;
	deliverable = pending & ~(signal_blocked_mask(current_task()) &
				  ~unblockable_mask());
	if (!deliverable)
		return 0;

	for (int sig = 1; sig <= NSIG; sig++) {
		if (deliverable & signal_mask(sig)) {
			*shared = (task_pending_mask(current_task()) &
				   signal_mask(sig)) == 0 &&
				  (shared_pending & signal_mask(sig)) != 0;
			return sig;
		}
	}

	return 0;
}

static struct sigaction get_signal_action(int sig)
{
	struct sigaction action;
	struct sighand_struct *sighand = task_sighand(current_task());

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
	struct sighand_struct *sighand = task_sighand(current_task());

	if (!sighand)
		return;

	mutex_lock(&sighand->lock);
	memset(&sighand->sigactions[sig], 0, sizeof(sighand->sigactions[sig]));
	mutex_unlock(&sighand->lock);
}

void do_signal(struct trap_frame *tf)
{
	while (true) {
		bool shared;
		bool forced;
		siginfo_t info;
		int sig = next_signal(&shared);

		if (sig == 0) {
			signal_restore_deferred_mask(current_task());
			return;
		}

		uint64_t mask = signal_mask(sig);
		struct sigaction action = get_signal_action(sig);
		__sighandler_t handler = action.sa_handler;
		forced = !shared &&
			 (current_task()->signal.forced_pending & mask) != 0;

		if (shared) {
			if (take_shared_pending(sig, &info) < 0)
				continue;
		} else {
			if (signal_pending_info(current_task(), sig, &info) < 0)
				continue;
			signal_clear_pending(current_task(), mask);
		}

		if (handler == SIG_IGN || handler == SIG_DFL)
			(void)restart_for_signal(current_task(), tf, false);
		if (!forced && handler == SIG_DFL &&
		    signal_init_default_ignored(current_task(), sig))
			continue;

		if (sig == SIGCONT) {
			if (handler == SIG_DFL || handler == SIG_IGN)
				continue;
		}

		if (signal_stops_default(sig, handler)) {
			stop_current(sig);
			continue;
		}

		if (sig == SIGKILL)
			do_exit_signal(sig);

		if (handler == SIG_IGN)
			continue;

		if (handler == SIG_DFL) {
			if (signal_is_fatal_default(sig))
				do_exit_signal(sig);
			continue;
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

static int kill_all_processes(int sig, const siginfo_t *info)
{
	bool found = false;
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
		if (sig == 0) {
			task_put(task);
			continue;
		}
		ret = send_group_signal_info(sig, info, task);
		task_put(task);
		if (ret < 0 && first_error == 0)
			first_error = ret;
	}

	if (!found)
		return -ESRCH;
	return first_error;
}

int do_kill(pid_t pid, int sig)
{
	struct task_struct *task;
	siginfo_t info = {0};
	long pgid;
	pid_t current_pgid;
	pid_t sid;
	int ret;

	if (sig != 0 && !signal_is_valid(sig))
		return -EINVAL;

	info.si_signo = sig;
	info.si_code = SI_USER;
	info.si_pid = current_task()->proc ? current_task()->proc->pid->nr : 0;
	info.si_uid = task_uid(current_task());
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
	if (sig == 0) {
		task_put(task);
		return 0;
	}

	ret = send_group_signal_info(sig, &info, task);

	task_put(task);
	return ret;
}

int do_tkill(pid_t tid, int sig)
{
	struct task_struct *task;
	siginfo_t info = {0};

	if (sig != 0 && !signal_is_valid(sig))
		return -EINVAL;
	if (tid <= 0)
		return -EINVAL;

	task = pid_lookup_task(tid);
	if (!task)
		return -ESRCH;
	if (sig == 0) {
		task_put(task);
		return 0;
	}

	info.si_signo = sig;
	info.si_code = SI_USER;
	info.si_pid = current_task()->proc ? current_task()->proc->pid->nr : 0;
	info.si_uid = task_uid(current_task());
	int ret = send_signal_info(sig, &info, task);

	task_put(task);
	return ret;
}

int do_tgkill(pid_t tgid, pid_t tid, int sig)
{
	struct proc_struct *proc;
	struct task_struct *task;
	siginfo_t info = {0};

	if (sig != 0 && !signal_is_valid(sig))
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
	if (sig == 0) {
		task_put(task);
		return 0;
	}

	info.si_signo = sig;
	info.si_code = SI_USER;
	info.si_pid = current_task()->proc ? current_task()->proc->pid->nr : 0;
	info.si_uid = task_uid(current_task());
	int ret = send_signal_info(sig, &info, task);

	task_put(task);
	return ret;
}

int do_sigaltstack(const struct stack_t *ss, struct stack_t *old_ss)
{
	struct stack_t *sas = task_altstack(current_task());

	if (old_ss)
		*old_ss = *sas;

	if (ss) {
		if (sas->ss_flags & SS_ONSTACK)
			return -EPERM;

		if (ss->ss_flags != 0 && ss->ss_flags != SS_DISABLE)
			return -EINVAL;

		if (ss->ss_flags & SS_DISABLE) {
			signal_reset_altstack(current_task());
		} else {
			if (ss->ss_size < MINSIGSTKSZ)
				return -ENOMEM;
			*sas = *ss;
			sas->ss_flags &= ~SS_ONSTACK;
		}
	}

	return 0;
}

int do_sigaction(int sig, const struct sigaction *act, struct sigaction *oldact)
{
	const unsigned long supported_flags =
		SA_SIGINFO | SA_ONSTACK | SA_RESTART | SA_NODEFER |
		SA_RESETHAND | SA_NOCLDSTOP | SA_NOCLDWAIT;
	struct sighand_struct *sighand = task_sighand(current_task());
	struct sigaction kact;

	if (!signal_is_valid(sig))
		return -EINVAL;
	if (!signal_is_catchable(sig) && act)
		return -EINVAL;

	if (oldact) {
		if (!sighand)
			return -EINVAL;

		mutex_lock(&sighand->lock);
		*oldact = sighand->sigactions[sig];
		mutex_unlock(&sighand->lock);
	}

	if (!act)
		return 0;

	kact = *act;
	if (kact.sa_handler == SIG_ERR)
		return -EINVAL;
	if (kact.sa_flags & ~supported_flags)
		return -EINVAL;

	kact.sa_mask &= ~unblockable_mask();
	if (!sighand)
		return -EINVAL;

	mutex_lock(&sighand->lock);
	sighand->sigactions[sig] = kact;
	mutex_unlock(&sighand->lock);
	return 0;
}

int do_sigprocmask(int how, const uint64_t *set, uint64_t *oldset)
{
	uint64_t newset;

	if (oldset)
		*oldset = signal_blocked_mask(current_task());

	if (!set)
		return 0;

	newset = *set & ~unblockable_mask();

	switch (how) {
	case SIG_BLOCK:
		signal_block_mask(current_task(), newset);
		break;
	case SIG_UNBLOCK:
		signal_unblock_mask(current_task(), newset);
		break;
	case SIG_SETMASK:
		signal_set_blocked_mask(current_task(), newset);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

ssize_t do_sigreturn(struct trap_frame *tf, uintptr_t sp)
{
	struct task_struct *task = current_task();
	struct rt_sigframe frame;
	struct rt_sigframe *user_frame = (struct rt_sigframe *)sp;
	const unsigned char *fp_state;
	struct signal_frame_state *state = signal_frame_top(current_task());

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
	signal_set_blocked_mask(current_task(), frame.uc.uc_sigmask);
	signal_frame_pop(current_task());
	*task_altstack(current_task()) = frame.uc.uc_stack;
	task->arch.tf = tf;
	return (ssize_t)trap_return_value(tf);
}

/*
 * Syscall-restart context. DESIGN.md assigns restart state to the task's
 * signal state; the signal module owns the policy and the dispatcher only
 * saves through these interfaces.
 */

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
