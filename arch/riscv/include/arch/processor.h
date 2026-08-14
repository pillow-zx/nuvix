#ifndef _NUVIX_ARCH_RISCV_PROCESSOR_H
#define _NUVIX_ARCH_RISCV_PROCESSOR_H

#include <nuvix/compiler.h>
#include <nuvix/types.h>
#include <asm/csr.h>

struct cpu;

__always_inline
static inline void wait_for_interrupt(void)
{
	wfi();
}

__always_inline
static inline void arch_sched_remote_wake(struct cpu *cpu)
{
	(void)cpu;
}

__always_inline __must_check
static inline uintptr_t trap_pc(void)
{
	return csr_read(sepc);
}

__always_inline __must_check
static inline uintptr_t trap_cause(void)
{
	return csr_read(scause);
}

__always_inline __must_check
static inline uintptr_t trap_value(void)
{
	return csr_read(stval);
}

__always_inline
static inline void flush_icache(void)
{
	icache_flush();
}

#endif
