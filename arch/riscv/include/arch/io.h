#ifndef _NUVIX_ARCH_RISCV_IO_H
#define _NUVIX_ARCH_RISCV_IO_H

#include <nuvix/compiler.h>

/* Order normal memory and device accesses in both directions. */
__always_inline
static inline void arch_io_mb(void)
{
	asm volatile("fence iorw,iorw" ::: "memory");
}

#endif
