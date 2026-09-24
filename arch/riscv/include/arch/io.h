#ifndef _NUVIX_ARCH_RISCV_IO_H
#define _NUVIX_ARCH_RISCV_IO_H

#include <nuvix/compiler.h>
#include <nuvix/types.h>

/* Boot-only mappings, before secondary CPUs or process roots exist. */
vaddr_t mmio_map(paddr_t address, size_t size);

/* Order normal memory and device accesses in both directions. */
__always_inline
static inline void arch_io_mb(void)
{
	asm volatile("fence iorw,iorw" ::: "memory");
}

#endif
