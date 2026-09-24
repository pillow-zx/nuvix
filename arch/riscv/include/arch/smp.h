#ifndef _NUVIX_ARCH_RISCV_SMP_H
#define _NUVIX_ARCH_RISCV_SMP_H

#include <nuvix/types.h>

void arch_smp_prepare(void);
int arch_cpu_start(uint32_t logical_id);
void arch_cpu_start_diagnose(uint32_t logical_id);
int smp_ipi_notify(uint32_t hartid);
void smp_ipi_ack(void);

#endif
