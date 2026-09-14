#ifndef _NUVIX_ARCH_RISCV_TASK_H
#define _NUVIX_ARCH_RISCV_TASK_H


#include <nuvix/types.h>
#include <nuvix/compiler.h>
#include <arch/config.h>
#include <arch/page.h>
#include <asm/asm_offsets.h>
#include <asm/context.h>
#include <asm/trap_frame.h>

struct task_struct;

struct task_state {
	struct context ctx;
	struct trap_frame *tf;
	void *kstack;
};

__nonnull(1, 2)
struct task_struct *arch_task_switch(struct task_struct *prev,
				     struct task_struct *next,
				     uintptr_t next_pgroot);

static_assert(ARCH_KSTACK_SIZE == TASK_KSTACK_SIZE,
	      "entry.S __trapret kstack arithmetic is out of sync");
static_assert((ARCH_KSTACK_SIZE - TRAP_FRAME_ALLOC_SIZE) % 16 == 0,
	      "kernel trap-frame allocation must preserve stack alignment");

#endif
