#ifndef _NUVIX_ARCH_RISCV_CPU_H
#define _NUVIX_ARCH_RISCV_CPU_H

#include <nuvix/compiler.h>

struct cpu;

__always_inline __must_check __returns_nonnull
static inline struct cpu *arch_current_cpu(void)
{
	struct cpu *cpu;

	asm volatile("mv %0, tp" : "=r"(cpu));
	return cpu;
}

__always_inline
static inline void arch_current_cpu_install(struct cpu *cpu)
{
	asm volatile("mv tp, %0" :: "r"(cpu));
}

#endif
