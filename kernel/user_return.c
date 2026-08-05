/*
 * kernel/user_return.c - generic user-return work boundary
 */

#include <kernel/user_return.h>
#include <kernel/exit.h>
#include <kernel/rseq.h>
#include <kernel/proc.h>
#include <kernel/signal.h>
#include <kernel/trap.h>
#include <uapi/signal.h>

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

	signal_write_child_tid(current_task());

	do_signal(tf);
	trap_disable_user_fpu(tf);
	/* Future syscall restart handling belongs here. */
	/* Future generic pending user-return work belongs here. */
}
