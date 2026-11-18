#ifndef _ASM_BARRIER_H
#define _ASM_BARRIER_H

#if defined(NUVIX_ARCH_RISCV)
#include <asm/riscv/barrier.h>
#else
#error "No supported Nuvix architecture selected"
#endif

#endif /* ifndef _ASM_BARRIER_H */
