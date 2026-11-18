#ifndef _ASM_IRQ_H
#define _ASM_IRQ_H

#if defined(NUVIX_ARCH_RISCV)
#include <asm/riscv/irq.h>
#else
#error "No supported Nuvix architecture selected"
#endif

#endif /* ifndef _ASM_IRQ_H */
