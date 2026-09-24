#ifndef _NUVIX_MMIO_H
#define _NUVIX_MMIO_H

#include <nuvix/compiler.h>
#include <nuvix/types.h>
#include <nuvix/barrier.h>

/* Boot-only mappings, before secondary CPUs or process roots exist. */
vaddr_t mmio_map(paddr_t address, size_t size);

/* Raw volatile accesses; callers supply device-specific ordering. */
#define MMIO_READ(type, addr) (*(volatile type *)(addr))

#define MMIO_WRITE(type, addr, val) (*(volatile type *)(addr) = (val))

__always_inline
static inline void mmio_mb(void)
{
	arch_io_mb();
}

#endif
