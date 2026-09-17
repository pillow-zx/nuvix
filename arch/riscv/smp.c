
#include <arch/sbi.h>
#include <arch/smp.h>
#include <arch/page.h>
#include <asm/csr.h>
#include <nuvix/cpu.h>
#include <nuvix/printk.h>
#include <nuvix/smp.h>
#include <nuvix/types.h>

extern char secondary_entry[];

uintptr_t smp_secondary_entry(void)
{
	return __pa((uintptr_t)&secondary_entry);
}

void smp_basic_prepare(void)
{
	struct sbi_ret ret;

	if (nr_cpu_ids > 1) {
		ret = sbi_probe_extension(SBI_EID_HSM);
		if (ret.error != 0 || ret.value == 0)
			panic("sbi: HSM extension required for %u CPUs but "
			      "not probed (error=%ld value=%ld)\n",
			      nr_cpu_ids, ret.error, ret.value);
		ret = sbi_probe_extension(SBI_EID_IPI);
		if (ret.error != 0 || ret.value == 0)
			panic("sbi: IPI extension required for %u CPUs but "
			      "not probed (error=%ld value=%ld)\n",
			      nr_cpu_ids, ret.error, ret.value);
		ret = sbi_probe_extension(SBI_EID_RFENCE);
		if (ret.error != 0 || ret.value == 0)
			panic("sbi: RFENCE extension required for %u CPUs but "
			      "not probed (error=%ld value=%ld)\n",
			      nr_cpu_ids, ret.error, ret.value);
	}

	for (uint32_t id = 0; id < nr_cpu_ids; id++) {
		uint32_t hartid = cpu_table[id].hartid;
		const char *name;

		if (id == 0)
			continue;

		ret = sbi_hsm_hart_get_status(hartid);
		name = sbi_hsm_status_name((uint64_t)ret.value);
		if (ret.error != 0 || ret.value != SBI_HSM_STOPPED)
			panic("sbi: hart %u (logical %u) not HSM STOPPED "
			      "(error=%ld value=%ld %s)\n",
			      hartid, id, ret.error, ret.value,
			      name ? name : "unknown");
	}
}

int smp_start_cpu(uint32_t hartid, uintptr_t entry_pa, uint32_t logical_id)
{
	struct sbi_ret ret;

	ret = sbi_hsm_hart_start(hartid, entry_pa, logical_id);
	return (int)ret.error;
}

struct smp_hart_status smp_hart_status(uint32_t hartid)
{
	struct sbi_ret ret = sbi_hsm_hart_get_status(hartid);

	return (struct smp_hart_status){.error = ret.error, .value = ret.value};
}

const char *smp_hart_status_name(uint64_t value)
{
	return sbi_hsm_status_name(value);
}

int smp_ipi_notify(uint32_t hartid)
{
	struct sbi_ret ret = sbi_ipi_send(1, hartid);

	return (int)ret.error;
}

void smp_ipi_ack(void)
{
	csr_clear(sip, SIP_SSIP);
}
