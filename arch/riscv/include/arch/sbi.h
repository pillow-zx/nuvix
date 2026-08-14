#ifndef _CUTEOS_ARCH_RISCV_SBI_H
#define _CUTEOS_ARCH_RISCV_SBI_H

/*
 * arch/riscv/include/arch/sbi.h - OpenSBI ecall protocol surface
 *
 * SBI v0.2 extension IDs and function IDs live here, never in generic
 * kernel code.
 */

#include <kernel/types.h>

#define SBI_EID_BASE		0x10
#define SBI_EID_HSM		0x48534D
#define SBI_EID_IPI		0x735049

#define SBI_FID_BASE_SPEC_VERSION 0
#define SBI_FID_BASE_PROBE_EXT	3
#define SBI_FID_HSM_HART_START	0
#define SBI_FID_HSM_HART_GET_STATUS 2
#define SBI_FID_IPI_SEND	0

/* SBI v0.2+ HSM hart states (hart_get_status value field). */
#define SBI_HSM_STARTED	      0
#define SBI_HSM_STOPPED	      1
#define SBI_HSM_START_PENDING 2
#define SBI_HSM_STOP_PENDING  3
#define SBI_HSM_SUSPENDED     4
#define SBI_HSM_RESUME_PENDING 5

struct sbi_ret {
	int64_t error;
	int64_t value;
};

void sbi_console_putchar(int ch);

struct sbi_ret sbi_base_spec_version(void);
struct sbi_ret sbi_probe_extension(uint64_t extension_id);
struct sbi_ret sbi_hsm_hart_start(uint64_t hartid, uint64_t start_addr,
				  uint64_t opaque);
struct sbi_ret sbi_hsm_hart_get_status(uint64_t hartid);
struct sbi_ret sbi_ipi_send(uint64_t hart_mask, uint64_t hart_mask_base);

/* Symbolic name for an HSM state value, or NULL when unknown. */
const char *sbi_hsm_status_name(uint64_t value);

#endif
