/*
 * arch/riscv/sbi.c - OpenSBI ecall 封装
 */

#include <arch/sbi.h>
#include <arch/system.h>
#include <nuvix/bootinfo.h>
#include <nuvix/types.h>

#define SBI_EID_CONSOLE_PUTCHAR 0x01
#define SBI_EID_SHUTDOWN	0x08
#define SBI_EID_SYSTEM_RESET	0x53525354
#define SBI_FID_SYSTEM_RESET	0

#define SBI_RESET_TYPE_SHUTDOWN	   0
#define SBI_RESET_TYPE_COLD_REBOOT 1
#define SBI_RESET_REASON_NONE	   0

static inline struct sbi_ret sbi_ecall(uint64_t eid, uint64_t fid,
				       uint64_t arg0, uint64_t arg1,
				       uint64_t arg2, uint64_t arg3,
				       uint64_t arg4)
{
	register long a0 __asm__("a0") = (long)arg0;
	register long a1 __asm__("a1") = (long)arg1;
	register long a2 __asm__("a2") = (long)arg2;
	register long a3 __asm__("a3") = (long)arg3;
	register long a4 __asm__("a4") = (long)arg4;
	register long a6 __asm__("a6") = (long)fid;
	register long a7 __asm__("a7") = (long)eid;

	__asm__ __volatile__("ecall"
			     : "+r"(a0), "+r"(a1)
			     : "r"(a2), "r"(a3), "r"(a4), "r"(a6), "r"(a7)
			     : "memory");

	return (struct sbi_ret){.error = a0, .value = a1};
}

void sbi_console_putchar(int ch)
{
	sbi_ecall(SBI_EID_CONSOLE_PUTCHAR, 0, (uint64_t)(unsigned char)ch, 0, 0,
		  0, 0);
}

struct sbi_ret sbi_base_spec_version(void)
{
	return sbi_ecall(SBI_EID_BASE, SBI_FID_BASE_SPEC_VERSION, 0, 0, 0, 0,
			 0);
}

struct sbi_ret sbi_probe_extension(uint64_t extension_id)
{
	return sbi_ecall(SBI_EID_BASE, SBI_FID_BASE_PROBE_EXT, extension_id, 0,
			 0, 0, 0);
}

struct sbi_ret sbi_hsm_hart_start(uint64_t hartid, uint64_t start_addr,
				  uint64_t opaque)
{
	return sbi_ecall(SBI_EID_HSM, SBI_FID_HSM_HART_START, hartid,
			 start_addr, opaque, 0, 0);
}

struct sbi_ret sbi_hsm_hart_get_status(uint64_t hartid)
{
	return sbi_ecall(SBI_EID_HSM, SBI_FID_HSM_HART_GET_STATUS, hartid, 0, 0,
			 0, 0);
}

struct sbi_ret sbi_ipi_send(uint64_t hart_mask, uint64_t hart_mask_base)
{
	return sbi_ecall(SBI_EID_IPI, SBI_FID_IPI_SEND, hart_mask,
			 hart_mask_base, 0, 0, 0);
}

/* Extensions nuvix reports in its boot banner, in display order. */
static const struct {
	uint64_t eid;
	const char *name;
} sbi_extensions[] = {
	{SBI_EID_BASE, "base"}, {SBI_EID_HSM, "hsm"},
	{SBI_EID_IPI, "ipi"},	{SBI_EID_TIME, "time"},
	{SBI_EID_RFNC, "rfnc"}, {SBI_EID_SYSTEM_RESET, "srst"},
};

BOOTINFO_BLOCK(
	sbi, void,

	const size_t nr_extensions = sizeof(sbi_extensions) /
				     sizeof(sbi_extensions[0]);
	struct sbi_ret ret = sbi_base_spec_version(); char extensions[128];
	size_t off = 0; size_t found = 0;

	if (ret.error != 0) {
		BROW("SBI Version", "unavailable (error=%lld)",
		     (long long)ret.error);
	} else {
		/* Spec version encodes major in the high byte and minor in
		 * the low 24 bits, e.g. SBI 3.0 -> 0x30000. */
		BROW("SBI Version", "%llu.%llu",
		     (unsigned long long)(ret.value >> 24),
		     (unsigned long long)(ret.value & 0xFFFFFF));
	}

	for (size_t i = 0; i < nr_extensions; i++) {
		ret = sbi_probe_extension(sbi_extensions[i].eid);
		if (ret.error != 0 || ret.value == 0)
			continue;
		off = bootinfo_append(extensions, sizeof(extensions), off,
				      "%s%s", found ? "," : "",
				      sbi_extensions[i].name);
		found++;
	}

	if (found == 0) BROW("SBI Extensions", "none");
	else BROW("SBI Extensions", "%s", extensions);)

const char *sbi_hsm_status_name(uint64_t value)
{
	switch (value) {
	case SBI_HSM_STARTED:
		return "STARTED";
	case SBI_HSM_STOPPED:
		return "STOPPED";
	case SBI_HSM_START_PENDING:
		return "START_PENDING";
	case SBI_HSM_STOP_PENDING:
		return "STOP_PENDING";
	case SBI_HSM_SUSPENDED:
		return "SUSPENDED";
	case SBI_HSM_RESUME_PENDING:
		return "RESUME_PENDING";
	default:
		return NULL;
	}
}

void system_reset(enum system_reset_mode mode)
{
	uint32_t type = mode == ARCH_SYSTEM_RESET_RESTART
				? SBI_RESET_TYPE_COLD_REBOOT
				: SBI_RESET_TYPE_SHUTDOWN;
	struct sbi_ret ret;

	ret = sbi_ecall(SBI_EID_SYSTEM_RESET, SBI_FID_SYSTEM_RESET, type,
			SBI_RESET_REASON_NONE, 0, 0, 0);
	if (ret.error != 0)
		sbi_ecall(SBI_EID_SHUTDOWN, 0, 0, 0, 0, 0, 0);

	for (;;)
		__asm__ __volatile__("wfi");
}
