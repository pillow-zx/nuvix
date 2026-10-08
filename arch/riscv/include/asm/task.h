#ifndef _NUVIX_RISCV_TASK_H
#define _NUVIX_RISCV_TASK_H

/* RV64 offsets from struct task; keep in sync with task and task_context. */
#define TASK_KSTACK  72
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
#define TASK_TRAP_T0 192
#define TASK_TRAP_T1 200

#ifdef CONFIG_FPU
#define TASK_CTX_FREG(n) (208 + (n) * __SIZEOF_LONG__)
#define TASK_CTX_FCSR    464
#define TASK_CTX_FS      472
#define TASK_CTX_SUM     480
#else
#define TASK_CTX_SUM 208
#endif

#ifndef __ASSEMBLER__

#include <nuvix/ctypes.h>

/* Kernel continuation and live FP state; user integers are in trap frames. */
struct task_context {
    unsigned long ra;
    unsigned long sp;
    unsigned long s[12];
    /* Temporary user-entry values, copied into the trap frame before C code. */
    unsigned long trap_t0;
    unsigned long trap_t1;
#ifdef CONFIG_FPU
    unsigned long fregs[32];
    unsigned long fcsr;
    unsigned long fs;
#endif
    unsigned long sum;
};

struct task;

/* IRQ off; returns the task just switched out, with tp bound to next. */
struct task *__switch_to(struct task *prev, struct task *next);

#endif /* __ASSEMBLER__ */

#endif /* _NUVIX_RISCV_TASK_H */
