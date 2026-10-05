#ifndef _NUVIX_RISCV_CURRENT_H
#define _NUVIX_RISCV_CURRENT_H

#include <nuvix/compiler.h>

struct task;

/* Borrowed pointer, valid only after this CPU has bound its current task. */
static __always_inline struct task *current_task(void)
{
    struct task *task;

    __asm__ volatile("mv %0, tp" : "=r"(task));
    return task;
}

#define current current_task()

#endif /* _NUVIX_RISCV_CURRENT_H */
