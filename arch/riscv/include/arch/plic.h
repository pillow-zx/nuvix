#ifndef _NUVIX_ARCH_RISCV_PLIC_H
#define _NUVIX_ARCH_RISCV_PLIC_H

#include <nuvix/config.h>
#include <nuvix/dt.h>

#define PLIC_MAX_SOURCES  1023u
#define PLIC_MAX_CONTEXTS 15872u

struct plic_config {
	struct dt_resource regs;
	uint32_t ndev;
	/* Indexed by logical CPU, not hart ID. */
	uint32_t contexts[NR_CPUS];
};

/* Boot CPU only, after CPU topology, slab and MMIO mapping are available,
 * before tasks/page tables or secondary CPUs are created. */
void plic_init(const struct plic_config *config);
/* Current CPU, with local IRQs disabled, before enabling SIE_SEIE. */
void plic_cpu_init(void);
void plic_handle_irq(void);

/* Controls notifications to an initialized logical CPU. Claim itself is
 * independent of threshold; a drain can also service lower-priority sources.
 * Unsupported WARL values return -EINVAL and preserve the old threshold. */
int plic_set_threshold(uint32_t cpu, uint32_t threshold);

#endif
