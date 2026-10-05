#ifndef _NUVIX_RISCV_TASK_H
#define _NUVIX_RISCV_TASK_H

/* RV64 offsets from struct task; keep in sync with task and task_context. */
#define TASK_CTX_RA  80
#define TASK_CTX_SP  88
#define TASK_CTX_S0  96
#define TASK_CTX_S1  104
#define TASK_CTX_S2  112
#define TASK_CTX_S3  120
#define TASK_CTX_S4  128
#define TASK_CTX_S5  136
#define TASK_CTX_S6  144
#define TASK_CTX_S7  152
#define TASK_CTX_S8  160
#define TASK_CTX_S9  168
#define TASK_CTX_S10 176
#define TASK_CTX_S11 184

#ifndef __ASSEMBLER__

#include <nuvix/ctypes.h>

/* Kernel continuation; user registers belong to the trap/return path. */
struct task_context {
    unsigned long ra;
    unsigned long sp;
    unsigned long s[12];
};

struct task;

/* IRQ off; returns the task just switched out, with tp bound to next. */
struct task *__switch_to(struct task *prev, struct task *next);

#endif /* __ASSEMBLER__ */

#endif /* _NUVIX_RISCV_TASK_H */
