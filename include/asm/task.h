#ifndef _ASM_TASK_H
#define _ASM_TASK_H

#if defined(NUVIX_ARCH_RISCV)
#include <asm/riscv/task.h>
#else
#error "No supported Nuvix architecture selected"
#endif

#endif /* ifndef _ASM_TASK_H */
