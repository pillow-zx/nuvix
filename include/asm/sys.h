#ifndef _ASM_SYS_H
#define _ASM_SYS_H

#if defined(NUVIX_ARCH_RISCV)
#include <asm/riscv/sys.h>
#else
#error "No supported Nuvix architecture selected"
#endif

#endif /* ifndef _ASM_SYS_H */
