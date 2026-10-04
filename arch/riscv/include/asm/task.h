#ifndef _NUVIX_RISCV_TASK_H
#define _NUVIX_RISCV_TASK_H

#include <nuvix/ctypes.h>

/* Kernel continuation; user registers belong to the trap/return path. */
struct task_context {
    unsigned long ra;
    unsigned long sp;
    unsigned long s[12];
};

#endif /* _NUVIX_RISCV_TASK_H */
