#ifndef _ASM_RISCV_TRAP_H
#define _ASM_RISCV_TRAP_H

#include <asm/riscv/csr.h>

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
    unsigned long fregs[32];
    unsigned long fcsr;
} __aligned(16);

#define syscallarg(frame, n) ((frame)->regs[10 + (n)])

void trap_handler(struct trap_frame *frame);

#endif /* __ASSEMBLER__ */

#endif /* _ASM_RISCV_TRAP_H */
