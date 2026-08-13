#ifndef _CUTEOS_KERNEL_SMP_H
#define _CUTEOS_KERNEL_SMP_H

/*
 * include/kernel/smp.h - CPU bring-up coordination
 *
 * The two module seams: the platform CPU-data module feeds compile-time
 * topology entries; the generic bring-up module validates, starts, and
 * waits for secondary harts. RISC-V SBI/HSM operations are the arch
 * adapter, never visible to init/main.c.
 */

/* Boot-error codes. Pure defines so boot.S may include this header. */
#define SMP_BOOT_ERR_NONE	    0
#define SMP_BOOT_ERR_HARTID_MISMATCH 1
#define SMP_BOOT_ERR_STATE_MISMATCH  2
#define SMP_BOOT_ERR_NO_SATP	    3
#define SMP_BOOT_ERR_HSM_START	    4

#ifndef __ASSEMBLER__

#include <kernel/types.h>
#include <arch/smp.h>

struct cpu_topology_entry;

/*
 * Platform CPU-data module: the sole owner of the hart enumeration. Returns
 * the first CONFIG_QEMU_CPUS entries of the QEMU virt table.
 */
const struct cpu_topology_entry *platform_cpu_entries(uint32_t *count);
uint32_t platform_boot_hartid(void);

/*
 * Generic bring-up. smp_prepare() validates the boot hart and topology;
 * smp_boot_cpus() starts secondaries and waits for ONLINE, panicking on any
 * mismatch or timeout. Both run on CPU 0 only.
 */
int smp_prepare(uint32_t boot_hartid);
void smp_boot_cpus(void);

/* Secondary entry from the assembly trampoline; never returns. */
__noreturn void smp_secondary_main(uint32_t hartid, uint32_t logical_id);

/* Per-CPU boot-error slot, writable by the pre-satp trampoline. */
extern uint32_t smp_boot_errors[];

#endif /* !__ASSEMBLER__ */
#endif
