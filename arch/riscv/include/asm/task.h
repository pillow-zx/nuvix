#ifndef _NUVIX_RISCV_TASK_H
#define _NUVIX_RISCV_TASK_H

/* RV64 offsets from struct task; keep in sync with task and task_context. */
#define TASK_KSTACK  40
#define TASK_CTX_RA  48
#define TASK_CTX_SP  56
#define TASK_CTX_S0  64
#define TASK_CTX_S1  72
#define TASK_CTX_S2  80
#define TASK_CTX_S3  88
#define TASK_CTX_S4  96
#define TASK_CTX_S5  104
#define TASK_CTX_S6  112
#define TASK_CTX_S7  120
#define TASK_CTX_S8  128
#define TASK_CTX_S9  136
#define TASK_CTX_S10 144
#define TASK_CTX_S11 152
#define TASK_CTX_FREG(n) (160 + (n) * __SIZEOF_LONG__)
#define TASK_CTX_FCSR    416
#define TASK_CTX_FS      424
#define TASK_CTX_SUM     432

#ifndef __ASSEMBLER__

#include <nuvix/compiler.h>
#include <nuvix/ctypes.h>
#include <nuvix/macro.h>

struct task_context {
    unsigned long ra;
    unsigned long sp;
    unsigned long s[12];
    unsigned long fregs[32];
    unsigned long fcsr;
    unsigned long fs;
    unsigned long sum;
};

struct task;
struct trap_frame;
struct mm_struct;

struct task *__switch_to(struct task *prev, struct task *next);

int arch_utask_init(struct task *task, struct mm_struct *mm, vaddr_t entry,
                         vaddr_t sp);
void arch_task_clone(struct task *task, const struct trap_frame *frame,
                    unsigned long sp);
void utask_entry(struct task *prev);
__noreturn void __utask_entry(void);

void arch_ktask_init(struct task_context *ctx, int (*fn)(void *), void *arg);
__noreturn void __ktask_entry(void);

static __always_inline struct task *current_task(void)
{
    struct task *task;

    __asm__ volatile("mv %0, tp" : "=r"(task));
    return task;
}

#define current current_task()

#endif /* __ASSEMBLER__ */

#endif /* _NUVIX_RISCV_TASK_H */
