/*
 * arch/riscv/trap.c - Trap 分发（C 层）
 */

#include <asm/csr.h>
#include <asm/trap.h>
#include <kernel/trap.h>
#include <kernel/printk.h>
#include <kernel/types.h>
#include <kernel/sched.h>
#include <kernel/task.h>
#include <kernel/timer.h>
#include <kernel/syscall.h>
#include <kernel/mm.h>
#include <kernel/exit.h>
#include <kernel/irq.h>
#include <kernel/signal.h>
#include <kernel/wait.h>
#include <kernel/user_return.h>

static const char *trap_origin(const struct trap_frame *tf)
{
	return trap_frame_from_user(tf) ? "user" : "kernel";
}

static siginfo_t trap_fault_info(int sig, int code, uintptr_t addr)
{
	siginfo_t info = {0};

	info.si_signo = sig;
	info.si_code = code;
	info.si_addr = (void *)addr;
	return info;
}

struct trap_exception trap_classify_exception(const struct trap_frame *tf)
{
	struct trap_exception exception = {0};
	uint64_t cause = trap_frame_cause(tf) & ~SCAUSE_IRQ_FLAG;

	if (cause == EXC_INST_PAGE_FAULT || cause == EXC_LOAD_PAGE_FAULT ||
	    cause == EXC_STORE_PAGE_FAULT) {
		exception.disposition = TRAP_EXCEPTION_PAGE_FAULT;
		return exception;
	}

	if (!trap_frame_from_user(tf)) {
		exception.disposition = TRAP_EXCEPTION_KERNEL_FATAL;
		return exception;
	}

	switch (cause) {
	case EXC_ECALL_U:
		exception.disposition = TRAP_EXCEPTION_SYSCALL;
		break;
	case EXC_INST_MISALIGNED:
	case EXC_LOAD_MISALIGNED:
	case EXC_STORE_MISALIGNED:
		exception.disposition = TRAP_EXCEPTION_USER_SIGNAL;
		exception.info = trap_fault_info(SIGBUS, BUS_ADRALN,
						 trap_fault_addr(tf));
		break;
	case EXC_INST_ACCESS:
	case EXC_LOAD_ACCESS:
	case EXC_STORE_ACCESS:
		exception.disposition = TRAP_EXCEPTION_USER_SIGNAL;
		exception.info = trap_fault_info(SIGSEGV, SEGV_ACCERR,
						 trap_fault_addr(tf));
		break;
	case EXC_INST_ILLEGAL:
		exception.disposition = TRAP_EXCEPTION_USER_SIGNAL;
		exception.info =
			trap_fault_info(SIGILL, ILL_ILLOPC, trap_user_pc(tf));
		break;
	case EXC_BREAKPOINT:
		exception.disposition = TRAP_EXCEPTION_USER_SIGNAL;
		exception.info =
			trap_fault_info(SIGTRAP, TRAP_BRKPT, trap_user_pc(tf));
		break;
	case EXC_ECALL_S:
		exception.disposition = TRAP_EXCEPTION_USER_SIGNAL;
		exception.info =
			trap_fault_info(SIGILL, ILL_ILLTRP, trap_user_pc(tf));
		break;
	default:
		exception.disposition = TRAP_EXCEPTION_USER_SIGNAL;
		exception.info =
			trap_fault_info(SIGILL, SI_KERNEL, trap_user_pc(tf));
		break;
	}

	return exception;
}

static void handle_timer_irq(void)
{
	clockevent_handle_irq(timer_now());
}

static void trap_user_return(struct trap_frame *tf)
{
	user_return_work(tf);
	if (current_task() && task_need_resched(current_task())) {
		if (irqs_disabled())
			schedule_irqoff();
		else
			schedule();
	}
	BUG_ON(!irqs_disabled());
}

void trap_handler(struct trap_frame *tf)
{
	struct task_struct *task = current_task();
	uint64_t scause = trap_frame_cause(tf);
	bool is_interrupt = (scause & SCAUSE_IRQ_FLAG) != 0;
	uint64_t code = scause & ~SCAUSE_IRQ_FLAG;
	bool user = trap_frame_from_user(tf);

	if (task)
		task->arch.tf = tf;

	if (is_interrupt) {
		irq_enter();
		switch (code) {
		case IRQ_S_TIMER:
			handle_timer_irq();
			irq_exit();
			if (user)
				trap_user_return(tf);
			return;
		default:
			irq_exit();
			panic("unhandled interrupt: origin=%s scause=0x%lx "
			      "code=%lu "
			      "sepc=%p stval=%p",
			      trap_origin(tf), (size_t)scause, (size_t)code,
			      (void *)trap_user_pc(tf),
			      (void *)trap_fault_addr(tf));
		}
	} else {
		struct trap_exception exception = trap_classify_exception(tf);

		switch (exception.disposition) {
		case TRAP_EXCEPTION_SYSCALL:
			trap_advance_pc(tf, 4);
			do_syscall(tf);
			if (user)
				trap_user_return(tf);
			return;
		case TRAP_EXCEPTION_PAGE_FAULT:
			do_page_fault(tf);
			if (user)
				trap_user_return(tf);
			return;
		case TRAP_EXCEPTION_USER_SIGNAL:
			if (exception.info.si_code == SI_KERNEL)
				pr_warn("unknown user exception: scause=0x%lx "
					"sepc=%p stval=%p pid=%d\n",
					(size_t)scause,
					(void *)trap_user_pc(tf),
					(void *)trap_fault_addr(tf),
					task->proc ? task->proc->pid->nr : 0);
			if (force_signal_info(exception.info.si_signo,
					      &exception.info, task) < 0)
				do_exit_signal(exception.info.si_signo);
			trap_user_return(tf);
			return;
		case TRAP_EXCEPTION_KERNEL_FATAL:
			panic("unhandled exception: origin=%s scause=0x%lx "
			      "code=%lu sepc=%p stval=%p ra=%p sp=%p "
			      "a0=%p a1=%p a2=%p",
			      trap_origin(tf), (size_t)scause, (size_t)code,
			      (void *)trap_user_pc(tf),
			      (void *)trap_fault_addr(tf), (void *)tf->ra,
			      (void *)tf->sp, (void *)tf->a0, (void *)tf->a1,
			      (void *)tf->a2);
		}
	}
}
