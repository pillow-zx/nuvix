#ifndef _NUVIX_ARCH_RISCV_TRAP_H
#define _NUVIX_ARCH_RISCV_TRAP_H

#include <nuvix/compiler.h>
#include <nuvix/types.h>
#include <nuvix/trap_types.h>
#include <nuvix/string.h>
#include <asm/csr.h>
#include <asm/context.h>
#include <asm/trap.h>
#include <asm/trap_frame.h>

struct task_struct;

void trap_cpu_init(void);

void trap_handler(struct trap_frame *tf);

struct trap_frame *trap_prepare_return(struct trap_frame *tf);

void __trapret(void);

__noreturn
void trapret_to_user(struct trap_frame *tf) ;


__must_check __pure __nonnull(1)
static inline size_t syscall_nr(const struct trap_frame *tf)
{
	return tf->a7;
}

__must_check __pure __nonnull(1)
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

__nonnull(1)
static inline void syscall_set_return(struct trap_frame *tf, ssize_t ret)
{
	tf->a0 = (size_t)ret;
}

__must_check __pure __nonnull(1)
static inline uintptr_t trap_user_sp(const struct trap_frame *tf)
{
	return tf->sp;
}

__must_check __pure __nonnull(1)
static inline uintptr_t trap_user_pc(const struct trap_frame *tf)
{
	return tf->sepc;
}

__must_check __pure __nonnull(1)
static inline uintptr_t trap_fault_addr(const struct trap_frame *tf)
{
	return tf->stval;
}

__must_check __pure __nonnull(1)
static inline bool trap_frame_from_user(const struct trap_frame *tf)
{
	return (tf->sstatus & SSTATUS_SPP) == 0;
}

__must_check __pure __nonnull(1)
static inline uintptr_t trap_frame_cause(const struct trap_frame *tf)
{
	return tf->scause;
}

__must_check __pure __nonnull(1)
static inline uintptr_t trap_status(const struct trap_frame *tf)
{
	return tf->sstatus;
}

__nonnull(1)
static inline void trap_set_status(struct trap_frame *tf, uintptr_t status)
{
	tf->sstatus = status;
}

__nonnull(1)
static inline void trap_disable_user_fpu(struct trap_frame *tf)
{
	tf->sstatus &= ~SSTATUS_FS_MASK;
}

__nonnull(1)
static inline void trap_advance_pc(struct trap_frame *tf, uintptr_t bytes)
{
	tf->sepc += bytes;
}

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

__nonnull(1)
static inline void trap_set_user_sp(struct trap_frame *tf, uintptr_t sp)
{
	tf->sp = sp;
}

__nonnull(1)
static inline void trap_set_user_pc(struct trap_frame *tf, uintptr_t pc)
{
	tf->sepc = pc;
}

__nonnull(1)
static inline void trap_set_arg0(struct trap_frame *tf, uintptr_t value)
{
	tf->a0 = value;
}

__nonnull(1)
static inline void trap_set_kernel_return(struct trap_frame *tf, uintptr_t pc)
{
	tf->sepc = pc;
	tf->sstatus |= SSTATUS_SPP | SSTATUS_SPIE;
}

__nonnull(1)
static inline void trap_set_kthread_frame(struct trap_frame *tf, uintptr_t pc, uintptr_t arg0)
{
	memset(tf, 0, sizeof(*tf));
	tf->sepc = pc;
	tf->a0 = arg0;
	tf->sstatus = SSTATUS_SPP | SSTATUS_SPIE;
}

__nonnull(1, 2)
static inline void trap_clone_frame(struct trap_frame *dst, const struct trap_frame *src)
{
	memcpy(dst, src, sizeof(*dst));
}

__nonnull(1)
static inline void trap_set_clone_return(struct trap_frame *tf)
{
	tf->a0 = 0;
}

__nonnull(1)
static inline void trap_set_tls(struct trap_frame *tf, uintptr_t tls)
{
	tf->tp = tls;
}

__must_check __pure __nonnull(1)
static inline  uintptr_t trap_return_value(const struct trap_frame *tf)
{
	return tf->a0;
}

__nonnull(1)
static inline void trap_setup_user_return(struct trap_frame *tf, uintptr_t pc, uintptr_t sp)
{
	tf->sepc = pc;
	tf->sp = sp;
	tf->sstatus = SSTATUS_SPIE;
}

#endif
