#ifndef _ASM_TLB_H
#define _ASM_TLB_H

#if defined(NUVIX_ARCH_RISCV)
#include <asm/riscv/tlb.h>
#else
#error "No supported Nuvix architecture selected"
#endif

#endif /* ifndef _ASM_TLB_H */
