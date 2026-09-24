#include <asm/setup.h>

#include <asm/sbi.h>
#include <arch/smp.h>
#include <arch/page.h>
#include <asm/csr.h>
#include <nuvix/cpu.h>
#include <nuvix/printk.h>
#include <nuvix/smp.h>
#include <nuvix/types.h>

extern char secondary_entry[];

void arch_smp_prepare(void)
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

int arch_cpu_start(uint32_t logical_id)
{
	uintptr_t token = (uintptr_t)atomic64_read_acquire(&pt_boot_token);

	BUG_ON((token & SATP_MODE_SV39) != SATP_MODE_SV39 ||
	       !(token & SATP_PPN_MASK));
	struct sbi_ret ret = sbi_hsm_hart_start(cpu_table[logical_id].hartid,
					      __pa(secondary_entry), logical_id);
	return (int)ret.error;
}

void arch_cpu_start_diagnose(uint32_t logical_id)
{
	uint32_t hartid = cpu_table[logical_id].hartid;
	struct sbi_ret status = sbi_hsm_hart_get_status(hartid);
	const char *name = sbi_hsm_status_name((uint64_t)status.value);

	pr_err("smp: hart %u HSM status error=%ld value=%ld %s\n",
	       hartid, status.error, status.value, name ? name : "unknown");
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
