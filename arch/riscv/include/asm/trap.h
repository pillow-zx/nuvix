#ifndef _NUVIX_RISCV_TRAP_H
#define _NUVIX_RISCV_TRAP_H

#include <asm/csr.h>

/* RV64 register slots; keep the assembly layout and C frame in agreement. */
#define TRAP_REG_SIZE 8
#define TRAP_REG(n)   ((n) * TRAP_REG_SIZE)
#define TRAP_SEPC     TRAP_REG(32)
#define TRAP_SSTATUS  TRAP_REG(33)
#define TRAP_SCAUSE   TRAP_REG(34)
#define TRAP_STVAL    TRAP_REG(35)
#define TRAP_SATP     TRAP_REG(36)
#define TRAP_TASK     TRAP_REG(37)

#ifdef CONFIG_FPU
#define TRAP_FREG(n) TRAP_REG(38 + (n))
#define TRAP_FCSR    TRAP_REG(70)
#define TRAP_SIZE    TRAP_REG(72)
#else
#define TRAP_SIZE TRAP_REG(38)
#endif

#ifndef __ASSEMBLER__

#include <nuvix/compiler.h>
#include <nuvix/macro.h>

struct task;

struct trap_frame {
    unsigned long regs[32];
    unsigned long sepc;
    unsigned long sstatus;
    unsigned long scause;
    unsigned long stval;
    unsigned long satp;
    /* Borrowed task pointer used to restore kernel tp on user entry. */
    struct task *task;
    IFDEF(CONFIG_FPU, unsigned long fregs[32]);
    IFDEF(CONFIG_FPU, unsigned long fcsr);
} __aligned(16);

#define syscallarg(frame, n) ((frame)->regs[10 + (n)])

void trap_handler(struct trap_frame *frame);

#endif /* __ASSEMBLER__ */

#endif /* _NUVIX_RISCV_TRAP_H */
