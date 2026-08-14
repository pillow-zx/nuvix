#ifndef _NUVIX_ARCH_RISCV_TRAP_H
#define _NUVIX_ARCH_RISCV_TRAP_H

#include <nuvix/compiler.h>
#include <nuvix/types.h>
#include <nuvix/trap_types.h>
#include <asm/csr.h>
#include <asm/context.h>
#include <asm/trap.h>
#include <asm/trap_frame.h>

/* Programs hart-local CSRs (stvec, sscratch, sie); CPU-local only. */
void trap_cpu_init(void);

void trap_handler(struct trap_frame *tf);
void __trapret(void);

__noreturn
void trapret_to_user(struct trap_frame *tf) ;
void switch_to(struct context *prev, struct context *next,
	       uintptr_t next_satp);

/**
 * @brief Read the Linux riscv64 syscall number from a trap frame.
 * @param tf User ecall trap frame.
 * @return Register a7, interpreted as the syscall number.
 */
__always_inline __must_check __pure __nonnull(1)
static inline size_t syscall_nr(const struct trap_frame *tf)
{
	return tf->a7;
}

/**
 * @brief Read a Linux riscv64 syscall argument from a trap frame.
 * @param tf User ecall trap frame.
 * @param nr Argument index in the range 0..5.
 * @return Registers a0..a5 according to the Linux riscv64 syscall ABI.
 */
__always_inline __must_check __pure __nonnull(1)
static inline size_t syscall_arg(const struct trap_frame *tf, uint32_t nr)
{
	switch (nr) {
	case 0:
		return tf->a0;
	case 1:
		return tf->a1;
	case 2:
		return tf->a2;
	case 3:
		return tf->a3;
	case 4:
		return tf->a4;
	case 5:
		return tf->a5;
	default:
		unreachable();
	}
}

/**
 * @brief Store a syscall return value into the ABI return register.
 * @param tf User ecall trap frame.
 * @param ret Signed syscall return value, including negative errno.
 */
__always_inline __nonnull(1)
static inline void syscall_set_return(struct trap_frame *tf, ssize_t ret)
{
	tf->a0 = (size_t)ret;
}

/**
 * @brief Return the user stack pointer saved in a trap frame.
 * @param tf Trap frame.
 * @return Saved sp value.
 */
__always_inline __must_check __pure __nonnull(1)
static inline uintptr_t trap_user_sp(const struct trap_frame *tf)
{
	return tf->sp;
}

/**
 * @brief Return the program counter saved in a trap frame.
 * @param tf Trap frame.
 * @return Saved sepc value.
 */
__always_inline __must_check __pure __nonnull(1)
static inline uintptr_t trap_user_pc(const struct trap_frame *tf)
{
	return tf->sepc;
}

/**
 * @brief Return the faulting virtual address from a trap frame.
 * @param tf Page-fault or access-fault trap frame.
 * @return Saved stval value.
 */
__always_inline __must_check __pure __nonnull(1)
static inline uintptr_t trap_fault_addr(const struct trap_frame *tf)
{
	return tf->stval;
}

/**
 * @brief Test whether a trap was taken from U-mode.
 * @param tf Trap frame.
 * @return True when SSTATUS_SPP is clear.
 */
__always_inline __must_check __pure __nonnull(1)
static inline bool trap_frame_from_user(const struct trap_frame *tf)
{
	return (tf->sstatus & SSTATUS_SPP) == 0;
}

/**
 * @brief Return the raw scause value saved in a trap frame.
 * @param tf Trap frame.
 * @return Raw scause, including SCAUSE_IRQ_FLAG for interrupts.
 */
__always_inline __must_check __pure __nonnull(1)
static inline uintptr_t trap_frame_cause(const struct trap_frame *tf)
{
	return tf->scause;
}

/**
 * @brief Return the saved sstatus value from a trap frame.
 * @param tf Trap frame.
 * @return Raw sstatus value.
 */
__always_inline __must_check __pure __nonnull(1)
static inline uintptr_t trap_status(const struct trap_frame *tf)
{
	return tf->sstatus;
}

/**
 * @brief Replace the saved sstatus value in a trap frame.
 * @param tf Trap frame.
 * @param status Raw sstatus value to restore on trap return.
 */
__always_inline __nonnull(1)
static inline void trap_set_status(struct trap_frame *tf, uintptr_t status)
{
	tf->sstatus = status;
}

/**
 * @brief Prevent the pending user return from enabling F/D instructions.
 * @param tf Trap frame whose sstatus will be restored by sret.
 */
__always_inline __nonnull(1)
static inline void trap_disable_user_fpu(struct trap_frame *tf)
{
	tf->sstatus &= ~SSTATUS_FS_MASK;
}

/**
 * @brief Advance the saved program counter.
 * @param tf Trap frame.
 * @param bytes Number of bytes to add to sepc.
 */
__always_inline __nonnull(1)
static inline void trap_advance_pc(struct trap_frame *tf, uintptr_t bytes)
{
	tf->sepc += bytes;
}

/**
 * @brief Classify a page/access fault by access type.
 * @param tf Trap frame whose scause encodes the fault.
 * @return Read, write, or execute access class for MM fault handling.
 */
__must_check __pure __nonnull(1)
static inline enum trap_access_type trap_fault_access(const struct trap_frame *tf)
{
	switch (tf->scause & ~SCAUSE_IRQ_FLAG) {
	case EXC_INST_PAGE_FAULT:
		return TRAP_ACCESS_EXEC;
	case EXC_LOAD_PAGE_FAULT:
	case EXC_INST_ACCESS:
		return TRAP_ACCESS_READ;
	case EXC_STORE_PAGE_FAULT:
		return TRAP_ACCESS_WRITE;
	default:
		return TRAP_ACCESS_READ;
	}
}

/**
 * @brief Return a short diagnostic name for a faulting access.
 * @param tf Trap frame whose scause encodes the fault.
 * @return Static string used in diagnostics.
 */
__must_check __pure __nonnull(1)
static inline const char *trap_fault_name(const struct trap_frame *tf)
{
	switch (tf->scause & ~SCAUSE_IRQ_FLAG) {
	case EXC_INST_PAGE_FAULT:
		return "instruction";
	case EXC_LOAD_PAGE_FAULT:
		return "load";
	case EXC_STORE_PAGE_FAULT:
		return "store";
	case EXC_INST_ACCESS:
		return "inst-access";
	default:
		return "unknown";
	}
}

/**
 * @brief Set the saved user stack pointer.
 * @param tf Trap frame.
 * @param sp User stack pointer to restore.
 */
__always_inline __nonnull(1)
static inline void trap_set_user_sp(struct trap_frame *tf, uintptr_t sp)
{
	tf->sp = sp;
}

/**
 * @brief Set the saved user program counter.
 * @param tf Trap frame.
 * @param pc User PC to restore.
 */
__always_inline __nonnull(1)
static inline void trap_set_user_pc(struct trap_frame *tf, uintptr_t pc)
{
	tf->sepc = pc;
}

/**
 * @brief Set register a0 in a trap frame.
 * @param tf Trap frame.
 * @param value Value written to a0.
 */
__always_inline __nonnull(1)
static inline void trap_set_arg0(struct trap_frame *tf, uintptr_t value)
{
	tf->a0 = value;
}

/**
 * @brief Prepare a trap frame to return to a supervisor-mode continuation.
 * @param tf Trap frame.
 * @param pc Supervisor PC to restore through sret.
 */
__always_inline __nonnull(1)
static inline void trap_set_kernel_return(struct trap_frame *tf, uintptr_t pc)
{
	tf->sepc = pc;
	tf->sstatus |= SSTATUS_SPP | SSTATUS_SPIE;
}

/**
 * @brief Initialize a trap frame for first entry into a kernel thread.
 * @param tf Trap frame storage at the top of the new kernel stack.
 * @param pc Kernel-thread entry function address.
 * @param arg0 First argument passed in a0.
 */
__always_inline __nonnull(1)
static inline void trap_set_kthread_frame(struct trap_frame *tf, uintptr_t pc, uintptr_t arg0)
{
	memset(tf, 0, sizeof(*tf));
	tf->sepc = pc;
	tf->a0 = arg0;
	tf->sstatus = SSTATUS_SPP | SSTATUS_SPIE;
}

/**
 * @brief Copy a parent register image into a child trap frame.
 * @param dst Destination child trap frame.
 * @param src Source parent trap frame.
 */
__always_inline __nonnull(1, 2)
static inline void trap_clone_frame(struct trap_frame *dst, const struct trap_frame *src)
{
	memcpy(dst, src, sizeof(*dst));
}

/**
 * @brief Set clone/fork child return value to zero.
 * @param tf Child trap frame.
 */
__always_inline __nonnull(1)
static inline void trap_set_clone_return(struct trap_frame *tf)
{
	tf->a0 = 0;
}

/**
 * @brief Set the thread pointer register used for Linux TLS.
 * @param tf Trap frame.
 * @param tls New tp value.
 */
__always_inline __nonnull(1)
static inline void trap_set_tls(struct trap_frame *tf, uintptr_t tls)
{
	tf->tp = tls;
}

/**
 * @brief Redirect user return into a signal handler.
 * @param tf Current user trap frame.
 * @param handler User signal handler PC.
 * @param restorer User restorer PC placed in ra.
 * @param sp User signal-frame stack pointer.
 * @param arg0 Signal number argument placed in a0.
 */
__always_inline __nonnull(1)
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

/**
 * @brief Read register a0 from a trap frame.
 * @param tf Trap frame.
 * @return Current a0 value.
 */
__always_inline __must_check __pure __nonnull(1)
static inline  uintptr_t trap_return_value(const struct trap_frame *tf)
{
	return tf->a0;
}

/**
 * @brief Prepare a trap frame for first return to U-mode.
 * @param tf Trap frame.
 * @param pc User entry PC.
 * @param sp User stack pointer.
 */
__always_inline __nonnull(1)
static inline void trap_setup_user_return(struct trap_frame *tf, uintptr_t pc, uintptr_t sp)
{
	tf->sepc = pc;
	tf->sp = sp;
	tf->sstatus = SSTATUS_SPIE;
}

#endif
