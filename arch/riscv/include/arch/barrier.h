#ifndef _NUVIX_ARCH_RISCV_BARRIER_H
#define _NUVIX_ARCH_RISCV_BARRIER_H

/**
 * @file barrier.h
 * @brief RISC-V memory-ordering fences.
 *
 * The fence assembly lives here and only here; callers above the arch layer
 * name the ordering guarantee they need, never the instruction.
 */

#include <nuvix/compiler.h>

/* Orders all prior loads and stores against all later loads and stores. */
__always_inline
static inline void arch_mb(void)
{
	asm volatile("fence rw,rw" ::: "memory");
}

/* Orders prior stores against later stores. */
__always_inline
static inline void arch_wmb(void)
{
	asm volatile("fence w,w" ::: "memory");
}

/* Orders prior loads against later loads. */
__always_inline
static inline void arch_rmb(void)
{
	asm volatile("fence r,r" ::: "memory");
}

#endif /* _NUVIX_ARCH_RISCV_BARRIER_H */
