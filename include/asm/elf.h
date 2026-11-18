#ifndef _ASM_ELF_H
#define _ASM_ELF_H

#if defined(NUVIX_ARCH_RISCV)
#include <asm/riscv/elf.h>
#else
#error "No supported Nuvix architecture selected"
#endif

#endif /* ifndef _ASM_ELF_H */
