#ifndef _NUVIX_ARCH_RISCV_TASK_H
#define _NUVIX_ARCH_RISCV_TASK_H

#include <nuvix/types.h>
#include <arch/config.h>
#include <asm/context.h>

struct trap_frame;

struct fpu_state {
	uint64_t registers[32];
	uint32_t fcsr;
};

struct task_state {
	struct context ctx;
	struct trap_frame *tf;
	void *kstack;
	struct fpu_state fpu;
};

#endif
