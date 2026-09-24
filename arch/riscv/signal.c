/* RISC-V signal frame, trampoline and register ABI. */
#include <arch/signal.h>
#include <nuvix/buddy.h>
#include <nuvix/errno.h>
#include <nuvix/mm.h>
#include <nuvix/processor.h>
#include <nuvix/signal.h>
#include <nuvix/task.h>
#include <nuvix/trap.h>
#include <uapi/syscall.h>
#include <uapi/mman.h>

#define RISCV_REG_ZERO 0
#define RISCV_REG_A7   17
#define RISCV_OP_IMM   0x13
#define RISCV_ECALL    0x00000073
#define RISCV_J_SELF   0x0000006f

#define RISCV_ADDI(rd, rs1, imm)                                               \
	((((uint32_t)(imm) & 0xfff) << 20) | ((uint32_t)(rs1) << 15) |         \
	 ((uint32_t)(rd) << 7) | RISCV_OP_IMM)

#define SIGNAL_TRAMPOLINE_ADDR (USER_STACK_GUARD_BASE - PAGE_SIZE)

static_assert(SYS_rt_sigreturn >= 0 && SYS_rt_sigreturn < 2048,
	      "SYS_sigreturn must fit in a RISC-V addi immediate");

struct rt_sigframe {
	siginfo_t info;
	struct ucontext uc;
};

static_assert(sizeof(struct rt_sigframe) == 1088,
	      "Linux riscv64 rt_sigframe size mismatch");

static void *trampoline_page;

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

static int signal_map_trampoline(struct mm_struct *mm)
{
	if (!trampoline_page)
		return -ENOMEM;

	return mm_install_fixed_page(mm, SIGNAL_TRAMPOLINE_ADDR,
				     trampoline_page, PROOT_READ | PROOT_EXEC);
}

int sig_mm_init(struct mm_struct *mm)
{
	return signal_map_trampoline(mm);
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

__nonnull(1)
static inline  void trap_setup_signal_handler(struct trap_frame *tf, uintptr_t handler, uintptr_t restorer,
	uintptr_t sp, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2)
{
	tf->sepc = handler;
	tf->ra = restorer;
	tf->sp = sp;
	tf->a0 = arg0;
	tf->a1 = arg1;
	tf->a2 = arg2;
}

int arch_signal_setup(struct trap_frame *tf, int sig,
			      const siginfo_t *info,
			      const struct sigaction *action, uint64_t mask)
{
	uintptr_t sp;
	struct rt_sigframe frame;
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
	memcpy(&frame.uc.uc_mcontext.sc_fpregs.d, &current_task()->arch.fpu,
	       sizeof(current_task()->arch.fpu));
	frame.uc.uc_sigmask = mask;

	if (copy_to_user((void *)sp, &frame, sizeof(frame)) != 0)
		return -EFAULT;

	if (on_altstack)
		sas->ss_flags |= SS_ONSTACK;

	trap_setup_signal_handler(tf, (uintptr_t)action->sa_handler,
				  SIGNAL_TRAMPOLINE_ADDR, sp, (uintptr_t)sig,
				  sp + offsetof(struct rt_sigframe, info),
				  sp + offsetof(struct rt_sigframe, uc));
	return 0;
}

int arch_signal_restore(struct trap_frame *tf, uintptr_t sp,
			uint64_t *mask, struct stack_t *stack)
{
	struct task_struct *task = current_task();
	struct rt_sigframe frame;
	struct rt_sigframe *user_frame = (struct rt_sigframe *)sp;
	if (copy_from_user(&frame, user_frame, sizeof(frame)) != 0)
		return -EFAULT;
	if (frame.uc.uc_flags != 0 || frame.uc.uc_link != NULL)
		return -EFAULT;
	if ((frame.uc.uc_mcontext.sc_regs.pc & 1) ||
	    !access_ok((const void *)frame.uc.uc_mcontext.sc_regs.pc, 1))
		return -EFAULT;
	if ((frame.uc.uc_mcontext.sc_regs.sp & 0xf) ||
	    frame.uc.uc_mcontext.sc_regs.sp == 0 ||
	    !access_ok((const void *)(frame.uc.uc_mcontext.sc_regs.sp - 1), 1))
		return -EFAULT;
	if (frame.uc.uc_stack.ss_flags != 0 &&
	    frame.uc.uc_stack.ss_flags != SS_DISABLE)
		return -EFAULT;
	if (!(frame.uc.uc_stack.ss_flags & SS_DISABLE) &&
	    (frame.uc.uc_stack.ss_size < MINSIGSTKSZ ||
	     !access_ok(frame.uc.uc_stack.ss_sp, frame.uc.uc_stack.ss_size)))
		return -EFAULT;
	if (frame.uc.uc_mcontext.sc_fpregs.ext.reserved ||
	    frame.uc.uc_mcontext.sc_fpregs.ext.hdr.magic ||
	    frame.uc.uc_mcontext.sc_fpregs.ext.hdr.size)
		return -EFAULT;
	memcpy(&task->arch.fpu, &frame.uc.uc_mcontext.sc_fpregs.d,
	       sizeof(current_task()->arch.fpu));
	task->arch.fpu.fcsr &= 0xff;

	signal_restore_user_regs(tf, &frame.uc.uc_mcontext.sc_regs);
	*mask = frame.uc.uc_sigmask;
	*stack = frame.uc.uc_stack;
	task->arch.tf = tf;
	return 0;
}

void sig_init(void)
{
	static const uint32_t code[] = {
		RISCV_ADDI(RISCV_REG_A7, RISCV_REG_ZERO, SYS_rt_sigreturn),
		RISCV_ECALL,
		RISCV_J_SELF,
	};

	if (trampoline_page)
		return;
	trampoline_page = get_page(0, ALLOC_NOWAIT);
	BUG_ON(!trampoline_page);
	memset(trampoline_page, 0, PAGE_SIZE);
	memcpy(trampoline_page, code, sizeof(code));
	flush_icache();
}
