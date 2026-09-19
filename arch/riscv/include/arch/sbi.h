#ifndef _NUVIX_ARCH_RISCV_SBI_H
#define _NUVIX_ARCH_RISCV_SBI_H


#include <nuvix/types.h>

#define SBI_EID_BASE			0x10
#define SBI_EID_HSM			0x48534D
#define SBI_EID_IPI			0x735049
#define SBI_EID_TIME			0x54494D45
#define SBI_EID_RFENCE			0x52464E43

#define SBI_FID_BASE_SPEC_VERSION	0
#define SBI_FID_BASE_PROBE_EXT		3
#define SBI_FID_HSM_HART_START		0
#define SBI_FID_HSM_HART_GET_STATUS	2
#define SBI_FID_IPI_SEND		0
#define SBI_FID_RFENCE_REMOTE_FENCE_I		0
#define SBI_FID_RFENCE_REMOTE_SFENCE_VMA	1

#define SBI_HSM_STARTED			0
#define SBI_HSM_STOPPED			1
#define SBI_HSM_START_PENDING		2
#define SBI_HSM_STOP_PENDING		3
#define SBI_HSM_SUSPENDED		4
#define SBI_HSM_RESUME_PENDING		5

struct sbi_ret {
	int64_t error;
	int64_t value;
};

struct sbi_ret sbi_set_timer(uint64_t deadline);

struct sbi_ret sbi_base_spec_version(void);
struct sbi_ret sbi_probe_extension(uint64_t extension_id);
#ifdef CONFIG_SMP
struct sbi_ret sbi_hsm_hart_start(uint64_t hartid, uint64_t start_addr,
				  uint64_t opaque);
struct sbi_ret sbi_hsm_hart_get_status(uint64_t hartid);
struct sbi_ret sbi_ipi_send(uint64_t hart_mask, uint64_t hart_mask_base);
struct sbi_ret sbi_remote_fence_i(uint64_t hart_mask,
				  uint64_t hart_mask_base);
struct sbi_ret sbi_remote_sfence_vma(uint64_t hart_mask,
				     uint64_t hart_mask_base,
				     uintptr_t start, size_t size);

const char *sbi_hsm_status_name(uint64_t value);
#endif

#endif
