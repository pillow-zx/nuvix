#ifndef _NUVIX_ARCH_RISCV_TASK_ACCESS_H
#define _NUVIX_ARCH_RISCV_TASK_ACCESS_H

/*
 * arch/riscv/include/arch/task_access.h - RISC-V task accessors
 */

#include <nuvix/compiler.h>
#include <nuvix/types.h>

static_assert(offsetof(struct task_struct, arch.kstack) == TASK_KSTACK,
		      "TASK_KSTACK offset in entry.S out of sync with task_struct");


__must_check __pure __nonnull(1) __returns_nonnull
static inline  struct trap_frame *task_kernel_tf(struct task_struct *task)
{
	uintptr_t frame = (uintptr_t)task->arch.kstack + KSTACK_SIZE -
			  TRAP_FRAME_ALLOC_SIZE;

	return (struct trap_frame *)frame;
}

__always_inline __must_check
static inline void *task_kernel_stack_take(struct task_struct *task)
{
	void *kstack;

	if (!task)
		return NULL;
	kstack = task->arch.kstack;
	task->arch.kstack = NULL;
	return kstack;
}

#endif
