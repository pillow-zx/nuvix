#ifndef _ASM_LAYOUT_H
#define _ASM_LAYOUT_H

#if defined(NUVIX_ARCH_RISCV)
#include <asm/riscv/layout.h>
#else
#error "No supported Nuvix architecture selected"
#endif

#endif /* ifndef _ASM_LAYOUT_H */
