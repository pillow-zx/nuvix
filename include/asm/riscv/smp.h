#ifndef _NUVIX_RISCV_SMP_H
#define _NUVIX_RISCV_SMP_H

/* Fixed RV64 layout of the HSM opaque argument, read before enabling MMU. */
#define CPU_BOOT_TASK  0
#define CPU_BOOT_STACK 8
#define CPU_BOOT_SATP  16

#ifndef __ASSEMBLER__

struct task;

struct cpu_boot_data {
    struct task *task;
    unsigned long stack;
    unsigned long satp;
};

#endif /* __ASSEMBLER__ */

#endif /* _NUVIX_RISCV_SMP_H */
