#ifndef _CUTEOS_ARCH_RISCV_SMP_H
#define _CUTEOS_ARCH_RISCV_SMP_H

/*
 * arch/riscv/include/arch/smp.h - RISC-V SMP bring-up adapter
 *
 * Implemented in arch/riscv/smp.c on top of the SBI BASE/HSM wrappers.
 */

struct smp_hart_status {
	int64_t error;
	int64_t value;
};

/* Validate SBI version, probe HSM/IPI for multi-CPU configs, and require
 * every secondary target to be HSM STOPPED. Panics on contract violation. */
void smp_arch_prepare(void);

/* Start one secondary hart at a physical address with a logical-id opaque. */
int smp_arch_start_cpu(uint32_t hartid, uintptr_t entry_pa, uint32_t logical_id);

/* Live HSM status of a hart; error/value pass through unchanged. */
struct smp_hart_status smp_arch_hart_status(uint32_t hartid);

/* Symbolic name for an HSM status value, or NULL when unknown. */
const char *smp_arch_hart_status_name(uint64_t value);

/* Physical entry address of the secondary trampoline. */
uintptr_t smp_arch_secondary_entry_pa(void);

#endif
