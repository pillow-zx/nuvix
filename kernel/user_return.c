/*
 * kernel/user_return.c - generic user-return work boundary
 */

#include <nuvix/user_return.h>
#include <nuvix/exit.h>
#include <nuvix/mm.h>
#include <nuvix/printk.h>
#include <nuvix/rseq.h>
#include <nuvix/proc.h>
#include <nuvix/signal.h>
#include <nuvix/trap.h>
#include <uapi/signal.h>

static void write_child_tid(struct task_struct *task)
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

void user_return_work(struct trap_frame *tf)
{
	int group_status;

	if (current_task()->proc &&
	    proc_group_exit_pending(current_task()->proc, &group_status)) {
		if (group_status & 0x7f)
			do_exit_signal(group_status & 0x7f);
		do_exit_group((group_status >> 8) & 0xff);
	}

	if (task_exec_exit_requested(current_task()))
		do_exit(0);

	if (rseq_resume_user(tf) < 0)
		do_exit_signal(SIGSEGV);

	write_child_tid(current_task());

	sig_deliver(tf);
	trap_disable_user_fpu(tf);
	/* Future syscall restart handling belongs here. */
	/* Future generic pending user-return work belongs here. */
}
