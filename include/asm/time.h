#ifndef _ASM_TIME_H
#define _ASM_TIME_H

#if defined(NUVIX_ARCH_RISCV)
#include <asm/riscv/time.h>
#else
#error "No supported Nuvix architecture selected"
#endif

#endif /* ifndef _ASM_TIME_H */
