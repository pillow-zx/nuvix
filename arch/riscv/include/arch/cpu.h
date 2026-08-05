#ifndef _CUTEOS_ARCH_RISCV_CPU_H
#define _CUTEOS_ARCH_RISCV_CPU_H

#include <asm/asm_offsets.h>

struct cpu;

__always_inline __must_check __pure __returns_nonnull
static inline struct cpu *arch_current_cpu(struct cpu *cpus)
{
	return cpus;
}

#endif
