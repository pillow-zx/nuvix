
#include <asm/entry.h>
#include <asm/asm_offsets.h>
#include <asm/csr.h>
#include <asm/trap.h>
#include <nuvix/trap.h>
#include <nuvix/printk.h>
#include <nuvix/types.h>
#include <nuvix/sched.h>
#include <nuvix/task.h>
#include <nuvix/proc.h>
#include <nuvix/timer.h>
#include <nuvix/syscall.h>
#include <nuvix/mm.h>
#include <nuvix/exit.h>
#include <nuvix/irq.h>
#include <nuvix/signal.h>
#include <nuvix/wait.h>
#include <nuvix/user_return.h>
#include <nuvix/ipi.h>
#include <asm/plic.h>

#define CHECK_TF_OFFSET(member, offset)                                        \
	static_assert(offsetof(struct trap_frame, member) == (offset),         \
		      "trap_frame." #member " offset mismatch")

CHECK_TF_OFFSET(sepc, TF_SEPC);
CHECK_TF_OFFSET(ra, TF_RA);
CHECK_TF_OFFSET(sp, TF_SP);
CHECK_TF_OFFSET(gp, TF_GP);
CHECK_TF_OFFSET(tp, TF_TP);
CHECK_TF_OFFSET(t0, TF_T0);
CHECK_TF_OFFSET(t1, TF_T1);
CHECK_TF_OFFSET(t2, TF_T2);
CHECK_TF_OFFSET(s0, TF_S0);
CHECK_TF_OFFSET(s1, TF_S1);
CHECK_TF_OFFSET(a0, TF_A0);
CHECK_TF_OFFSET(a1, TF_A1);
CHECK_TF_OFFSET(a2, TF_A2);
CHECK_TF_OFFSET(a3, TF_A3);
CHECK_TF_OFFSET(a4, TF_A4);
CHECK_TF_OFFSET(a5, TF_A5);
CHECK_TF_OFFSET(a6, TF_A6);
CHECK_TF_OFFSET(a7, TF_A7);
CHECK_TF_OFFSET(s2, TF_S2);
CHECK_TF_OFFSET(s3, TF_S3);
CHECK_TF_OFFSET(s4, TF_S4);
CHECK_TF_OFFSET(s5, TF_S5);
CHECK_TF_OFFSET(s6, TF_S6);
CHECK_TF_OFFSET(s7, TF_S7);
CHECK_TF_OFFSET(s8, TF_S8);
CHECK_TF_OFFSET(s9, TF_S9);
CHECK_TF_OFFSET(s10, TF_S10);
CHECK_TF_OFFSET(s11, TF_S11);
CHECK_TF_OFFSET(t3, TF_T3);
CHECK_TF_OFFSET(t4, TF_T4);
CHECK_TF_OFFSET(t5, TF_T5);
CHECK_TF_OFFSET(t6, TF_T6);
CHECK_TF_OFFSET(scause, TF_SCAUSE);
CHECK_TF_OFFSET(stval, TF_STVAL);
CHECK_TF_OFFSET(sstatus, TF_SSTATUS);
static_assert(sizeof(struct trap_frame) == TRAP_FRAME_SIZE,
	      "trap_frame size mismatch");
static_assert(TRAP_FRAME_ALLOC_SIZE % 16 == 0,
	      "trap-frame allocation must preserve stack alignment");

#undef CHECK_TF_OFFSET

void trap_cpu_init(void)
{
	csr_clear(sie, SIE_SEIE);
	csr_write(stvec, __alltraps);
	csr_write(sscratch, 0);
	csr_set(sie, SIE_STIE);
	csr_clear(sip, SIP_SSIP);
#ifdef CONFIG_SMP
	csr_set(sie, SIE_SSIE);
#else
	csr_clear(sie, SIE_SSIE);
#endif
	plic_cpu_init();
	csr_set(sie, SIE_SEIE);
}

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
	/* Signal delivery and child-TID stores may fault, sleep, or shoot down
	 * remote translations. Keep IPIs serviceable until the final return. */
	for (;;) {
		local_irq_enable();
		user_return_work(tf);
		local_irq_disable();
		if (!task_need_resched(current_task()))
			break;
		schedule_irqoff();
	}
	fpu_restore(&current_task()->arch.fpu);
	tf->sstatus = (tf->sstatus & ~SSTATUS_FS_MASK) | (2UL << SSTATUS_FS_SHIFT);
	BUG_ON(!irqs_disabled());
}

struct trap_frame *trap_prepare_return(struct trap_frame *tf)
{
	if (trap_frame_from_user(tf))
		trap_user_return(tf);
	return tf;
}

void trap_handler(struct trap_frame *tf)
{
	struct task_struct *task = current_task();
	uint64_t scause = trap_frame_cause(tf);
	bool is_interrupt = (scause & SCAUSE_IRQ_FLAG) != 0;
	uint64_t code = scause & ~SCAUSE_IRQ_FLAG;
	bool user = trap_frame_from_user(tf);

	/* Nested kernel interrupts must not replace the user register frame
	 * used by fork, signals, and user-return work. */
	if (task && user) {
		task->arch.tf = tf;
		fpu_save(&task->arch.fpu);
	}

	if (is_interrupt) {
		irq_enter();
		switch (code) {
#ifdef CONFIG_SMP
		case IRQ_S_SOFT:
			ipi_handle();
			break;
#endif
		case IRQ_S_TIMER:
			handle_timer_irq();
			break;
		case IRQ_S_EXT:
			plic_handle_irq();
			break;
		default:
			irq_exit();
			panic("unhandled interrupt: origin=%s scause=0x%lx "
			      "code=%lu "
			      "sepc=%p stval=%p",
			      trap_origin(tf), (size_t)scause, (size_t)code,
			      (void *)trap_user_pc(tf),
			      (void *)trap_fault_addr(tf));
		}
		irq_exit();
		if (user)
			trap_user_return(tf);
		else if (sched_context_can_schedule() && task_need_resched(task))
			schedule_irqoff();
		return;
	} else {
		if (!user && uaccess_fixup(tf))
			return;

		struct trap_exception exception = trap_classify_exception(tf);

		/* User exceptions run in sleepable task context. Concurrent page
		 * faults may block on MM locks and invoke synchronous remote-fence
		 * services. Kernel faults retain their entry IRQ state and uaccess
		 * fixups have already returned above. */
		if (user)
			local_irq_enable();

		switch (exception.disposition) {
		case TRAP_EXCEPTION_SYSCALL:
			trap_advance_pc(tf, 4);
			do_syscall(tf);
			if (user)
				trap_user_return(tf);
			else if (sched_context_can_schedule() && task_need_resched(task))
				schedule_irqoff();
			return;
		case TRAP_EXCEPTION_PAGE_FAULT:
			do_page_fault(tf);
			if (user)
				trap_user_return(tf);
			else if (sched_context_can_schedule() && task_need_resched(task))
				schedule_irqoff();
			return;
		case TRAP_EXCEPTION_USER_SIGNAL:
			if (exception.info.si_code == SI_KERNEL)
				pr_warn("unknown user exception: scause=0x%lx "
					"sepc=%p stval=%p pid=%d\n",
					(size_t)scause,
					(void *)trap_user_pc(tf),
					(void *)trap_fault_addr(tf),
					task->proc ? task->proc->pid->nr : 0);
			if (sig_force_info(exception.info.si_signo,
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
