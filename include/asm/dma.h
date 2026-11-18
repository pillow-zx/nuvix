#ifndef _ASM_DMA_H
#define _ASM_DMA_H

#if defined(NUVIX_ARCH_RISCV)
#include <asm/riscv/dma.h>
#else
#error "No supported Nuvix architecture selected"
#endif

#endif /* ifndef _ASM_DMA_H */
