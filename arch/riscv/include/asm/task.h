#ifndef _NUVIX_RISCV_TASK_H
#define _NUVIX_RISCV_TASK_H

/* RV64 offsets from struct task; keep in sync with task and task_context. */
#define TASK_KSTACK  56
#define TASK_CTX_RA  64
#define TASK_CTX_SP  72
#define TASK_CTX_S0  80
#define TASK_CTX_S1  88
#define TASK_CTX_S2  96
#define TASK_CTX_S3  104
#define TASK_CTX_S4  112
#define TASK_CTX_S5  120
#define TASK_CTX_S6  128
#define TASK_CTX_S7  136
#define TASK_CTX_S8  144
#define TASK_CTX_S9  152
#define TASK_CTX_S10 160
#define TASK_CTX_S11 168
#ifdef CONFIG_FPU
#define TASK_CTX_FREG(n) (176 + (n) * __SIZEOF_LONG__)
#define TASK_CTX_FCSR    432
#define TASK_CTX_FS      440
#define TASK_CTX_SUM     448
#else
#define TASK_CTX_SUM 176
#endif

#ifndef __ASSEMBLER__

#include <nuvix/compiler.h>
#include <nuvix/ctypes.h>
#include <nuvix/macro.h>

struct task_context {
    unsigned long ra;
    unsigned long sp;
    unsigned long s[12];
    IFDEF(CONFIG_FPU, unsigned long fregs[32]);
    IFDEF(CONFIG_FPU, unsigned long fcsr);
    IFDEF(CONFIG_FPU, unsigned long fs);
    unsigned long sum;
};

struct task;

struct task *__switch_to(struct task *prev, struct task *next);

__noreturn void __ktask_entry(void);

/*
 * First entry of a kernel task: ra = __kthread_entry with fn/arg in s0/s1,
 * delivered to kthread_entry(prev, fn, arg) on the first switch.
 */
static inline void arch_ktask_init(struct task_context *ctx,
                                     int (*fn)(void *), void *arg)
{
    ctx->ra = (unsigned long)__ktask_entry;
    ctx->s[0] = (unsigned long)fn;
    ctx->s[1] = (unsigned long)arg;
}

#endif /* __ASSEMBLER__ */

#endif /* _NUVIX_RISCV_TASK_H */
