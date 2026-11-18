#ifndef _ASM_CPU_H
#define _ASM_CPU_H

#if defined(NUVIX_ARCH_RISCV)
#include <asm/riscv/cpu.h>
#else
#error "No supported Nuvix architecture selected"
#endif

#endif /* ifndef _ASM_CPU_H */
