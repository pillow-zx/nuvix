#ifndef _NUVIX_ARCH_RISCV_CPU_H
#define _NUVIX_ARCH_RISCV_CPU_H

#include <asm/asm_offsets.h>

struct cpu;

/*
 * S-mode tp always carries the current struct cpu *. The kernel never uses
 * tp for anything else; user TLS lives in tp only while in U-mode. This read
 * must be volatile: the compiler may not assume tp stays untouched.
 */
__always_inline __must_check __returns_nonnull
static inline struct cpu *arch_current_cpu(void)
{
	struct cpu *cpu;

	asm volatile("mv %0, tp" : "=r"(cpu));
	return cpu;
}

#endif
