#ifndef _CUTEOS_ARCH_RISCV_TASK_H
#define _CUTEOS_ARCH_RISCV_TASK_H

/**
 * @file
 * @brief RISC-V task architecture state.
 */

#include <kernel/types.h>
#include <kernel/compiler.h>
#include <arch/page.h>
#include <asm/asm_offsets.h>
#include <asm/context.h>
#include <asm/trap_frame.h>

struct task_struct;

/** @def ARCH_KSTACK_ORDER
 * @brief Buddy allocation order for one kernel stack.
 */
#define ARCH_KSTACK_ORDER 2
/** @def ARCH_KSTACK_SIZE
 * @brief Kernel stack size in bytes for each task.
 */
#define ARCH_KSTACK_SIZE (PAGE_SIZE << ARCH_KSTACK_ORDER)

/**
 * @struct task_state
 * @brief RISC-V-owned task state embedded in struct task_struct.
 *
 * @par Fields
 * - @c ctx: Callee-saved kernel context for switch.S.
 * - @c tf: Current trap frame while running in kernel.
 * - @c kstack: Base address of the task kernel stack allocation.
 * - @c satp: User page-table root installed before U-mode return.
 */
struct task_state {
	struct context ctx;
	struct trap_frame *tf;
	void *kstack;
	uint64_t pgroot;
};

/** Architecture handoff for two scheduler-selected tasks. */
__nonnull(1, 2)
void arch_task_switch(struct task_struct *prev, struct task_struct *next);

static_assert(ARCH_KSTACK_SIZE == TASK_KSTACK_SIZE,
	      "entry.S __trapret kstack arithmetic is out of sync");
static_assert((ARCH_KSTACK_SIZE - TRAP_FRAME_ALLOC_SIZE) % 16 == 0,
	      "kernel trap-frame allocation must preserve stack alignment");

#endif
