#ifndef _ASM_SYSCALL_H
#define _ASM_SYSCALL_H

#if defined(NUVIX_ARCH_RISCV)
#include <asm/riscv/syscall.h>
#else
#error "No supported Nuvix architecture selected"
#endif

#endif /* ifndef _ASM_SYSCALL_H */
