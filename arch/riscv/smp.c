/*
 * arch/riscv/smp.c - RISC-V SMP bring-up adapter
 *
 * Policy on top of the raw SBI BASE/HSM wrappers: version requirements,
 * extension probing, and HSM-STOPPED acceptance for every secondary
 * target. Generic code never sees SBI constants.
 */

#include <arch/sbi.h>
#include <arch/smp.h>
#include <arch/page.h>
#include <kernel/cpu.h>
#include <kernel/printk.h>
#include <kernel/types.h>

extern char secondary_entry[];

uintptr_t smp_arch_secondary_entry_pa(void)
{
	return __pa((uintptr_t)&secondary_entry);
}
