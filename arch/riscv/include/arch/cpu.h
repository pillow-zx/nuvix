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

/*
 * Install this hart's CPU slot as the first statement of S-mode entry.  The
 * volatile asm orders the tp write before any arch_current_cpu() read.
 */
__always_inline
static inline void arch_current_cpu_install(struct cpu *cpu)
{
	asm volatile("mv tp, %0" :: "r"(cpu));
}

#endif
