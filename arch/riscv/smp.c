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
#include <asm/csr.h>
#include <kernel/cpu.h>
#include <kernel/printk.h>
#include <kernel/smp.h>
#include <kernel/types.h>

extern char secondary_entry[];

uintptr_t smp_arch_secondary_entry_pa(void)
{
	return __pa((uintptr_t)&secondary_entry);
}

void smp_arch_prepare(void)
{
	struct sbi_ret ret;

	ret = sbi_base_spec_version();
	if (ret.error != 0 || ret.value < 0x20000)
		panic("sbi: BASE required but unavailable (error=%lld "
		      "version=0x%llx)\n", ret.error, ret.value);

	if (nr_cpu_ids > 1) {
		ret = sbi_probe_extension(SBI_EID_HSM);
		if (ret.error != 0 || ret.value == 0)
			panic("sbi: HSM extension required for %u CPUs but "
			      "not probed (error=%lld value=%lld)\n",
			      nr_cpu_ids, ret.error, ret.value);
		ret = sbi_probe_extension(SBI_EID_IPI);
		if (ret.error != 0 || ret.value == 0)
			panic("sbi: IPI extension required for %u CPUs but "
			      "not probed (error=%lld value=%lld)\n",
			      nr_cpu_ids, ret.error, ret.value);
	}

	/* Every secondary target must be HSM STOPPED before any start; any
	 * other state is a fatal platform-contract failure. The boot hart is
	 * running (STARTED) by definition and is not a target. */
	for (uint32_t id = 0; id < nr_cpu_ids; id++) {
		uint32_t hartid = cpu_table[id].hartid;
		const char *name;

		if (id == 0)
			continue;

		ret = sbi_hsm_hart_get_status(hartid);
		name = sbi_hsm_status_name((uint64_t)ret.value);
		if (ret.error != 0 || ret.value != SBI_HSM_STOPPED)
			panic("sbi: hart %u (logical %u) not HSM STOPPED "
			      "(error=%lld value=%lld %s)\n",
			      hartid, id, ret.error, ret.value,
			      name ? name : "unknown");
	}
}

int smp_arch_start_cpu(uint32_t hartid, uintptr_t entry_pa, uint32_t logical_id)
{
	struct sbi_ret ret;

	ret = sbi_hsm_hart_start(hartid, entry_pa, logical_id);
	return (int)ret.error;
}

struct smp_hart_status smp_arch_hart_status(uint32_t hartid)
{
	struct sbi_ret ret = sbi_hsm_hart_get_status(hartid);

	return (struct smp_hart_status){.error = ret.error, .value = ret.value};
}

const char *smp_arch_hart_status_name(uint64_t value)
{
	return sbi_hsm_status_name(value);
}

int smp_arch_ipi_notify(uint32_t hartid)
{
	struct sbi_ret ret = sbi_ipi_send(1, hartid);

	return (int)ret.error;
}

void smp_arch_ipi_ack(void)
{
	/* QEMU virt asserts sip.SSIP by writing 1 (not W1C), so the
	 * acknowledgement writes it to zero. */
	csr_clear(sip, SIP_SSIP);
}
