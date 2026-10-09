#ifndef _NUVIX_RISCV_DMA_H
#define _NUVIX_RISCV_DMA_H

#include <nuvix/ctypes.h>
#include <nuvix/compiler.h>

/* QEMU virt's coherent direct-DMA profile uses RAM physical addresses. */
static __always_inline uint64_t arch_phys_to_dma(paddr_t paddr)
{
    return paddr;
}

#endif /* _NUVIX_RISCV_DMA_H */
