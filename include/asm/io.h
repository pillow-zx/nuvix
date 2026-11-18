#ifndef _ASM_IO_H
#define _ASM_IO_H

#if defined(NUVIX_ARCH_RISCV)
#include <asm/riscv/io.h>
#else
#error "No supported Nuvix architecture selected"
#endif

#endif /* ifndef _ASM_IO_H */
