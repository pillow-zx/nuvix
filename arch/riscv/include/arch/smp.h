#ifndef _NUVIX_ARCH_RISCV_SMP_H
#define _NUVIX_ARCH_RISCV_SMP_H


struct smp_hart_status {
	int64_t error;
	int64_t value;
};

void smp_basic_prepare(void);

int smp_start_cpu(uint32_t hartid, uintptr_t entry_pa, uint32_t logical_id);

struct smp_hart_status smp_hart_status(uint32_t hartid);

const char *smp_hart_status_name(uint64_t value);

uintptr_t smp_secondary_entry_pa(void);

int smp_ipi_notify(uint32_t hartid);

void smp_ipi_ack(void);

#endif
