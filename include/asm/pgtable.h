#ifndef _ASM_PGTABLE_H
#define _ASM_PGTABLE_H

#if defined(NUVIX_ARCH_RISCV)
#include <asm/riscv/pgtable.h>
#else
#error "No supported Nuvix architecture selected"
#endif

#endif /* ifndef _ASM_PGTABLE_H */
