#ifndef _ASM_UACCESS_H
#define _ASM_UACCESS_H

#if defined(NUVIX_ARCH_RISCV)
#include <asm/riscv/uaccess.h>
#else
#error "No supported Nuvix architecture selected"
#endif

#endif /* ifndef _ASM_UACCESS_H */
