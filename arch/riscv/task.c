/*
 * arch/riscv/task.c - RISC-V task context helpers
 */

#include <kernel/mm.h>
#include <kernel/task.h>
#include <kernel/tools.h>
#include <uapi/sched.h>
#include <arch/pgtable.h>
#include <arch/trap.h>

__must_check __pure
static uintptr_t task_pgroot(const struct task_struct *task)
{
	uint64_t satp = task->arch.pgroot;

	return satp ? satp : kernel_pgroot();
}

void arch_task_init(struct task_struct *task)
{
	task->arch.ctx.ra = 0;
	task->arch.ctx.sp = 0;
	task->arch.tf = NULL;
	task->arch.pgroot = 0;
}

void task_setup_kthread(struct task_struct *task, void (*fn)(void *),
				   void *arg)
{
	struct trap_frame *tf = task_kernel_tf(task);

	trap_set_kthread_frame(tf, (uintptr_t)fn, (uintptr_t)arg);

	task->arch.tf = tf;
	task->arch.ctx.ra = (size_t)__trapret;
	task->arch.ctx.sp = (size_t)tf;
}

void task_setup_clone_frame(struct task_struct *child,
				 const struct trap_frame *parent_tf,
				 unsigned long flags, uintptr_t child_stack,
				 uintptr_t tls)
{
	struct trap_frame *child_tf = task_kernel_tf(child);

	trap_clone_frame(child_tf, parent_tf);
	trap_set_clone_return(child_tf);
	if (child_stack != 0)
		trap_set_user_sp(child_tf, child_stack);
	if (flags & CLONE_SETTLS)
		trap_set_tls(child_tf, tls);

	child->arch.tf = child_tf;
	child->arch.ctx.ra = (size_t)__trapret;
	child->arch.ctx.sp = (size_t)child_tf;
}

void arch_task_switch(struct task_struct *prev, struct task_struct *next)
{
	switch_to(&prev->arch.ctx, &next->arch.ctx, task_pgroot(next));
}

bool task_trap_frome_user(const struct task_struct *task)
{
	const struct trap_frame *tf = task->arch.tf;

	return tf && trap_frame_from_user(tf);
}
