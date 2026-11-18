#ifndef _ASM_TRAP_H
#define _ASM_TRAP_H

#if defined(NUVIX_ARCH_RISCV)
#include <asm/riscv/trap.h>
#else
#error "No supported Nuvix architecture selected"
#endif

#endif /* ifndef _ASM_TRAP_H */
