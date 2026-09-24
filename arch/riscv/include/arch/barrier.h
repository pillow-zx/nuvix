#ifndef _NUVIX_ARCH_RISCV_BARRIER_H
#define _NUVIX_ARCH_RISCV_BARRIER_H
#include <nuvix/compiler.h>

__always_inline
static inline void arch_mb(void)
{
        asm volatile("fence rw,rw" ::: "memory");
}

__always_inline
static inline void arch_wmb(void)
{
        asm volatile("fence w,w" ::: "memory");
}

__always_inline
static inline void arch_rmb(void)
{
        asm volatile("fence r,r" ::: "memory");
}

__always_inline
static inline void arch_io_mb()
{
        asm volatile("fence iorw,iorw" ::: "memory");
}

#endif
