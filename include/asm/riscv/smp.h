#ifndef _NUVIX_RISCV_SMP_H
#define _NUVIX_RISCV_SMP_H

#ifndef __ASSEMBLER__

struct task;

/* HSM opaque argument, read before enabling the MMU. */
struct cpu_boot_data {
    struct task *task;
    unsigned long stack;
    unsigned long satp;
};

#endif /* __ASSEMBLER__ */

#endif /* _NUVIX_RISCV_SMP_H */
