#ifndef _NUVIX_SMP_H
#define _NUVIX_SMP_H

/*
 * include/nuvix/smp.h - CPU bring-up coordination
 *
 * The two module seams: the platform CPU-data module feeds compile-time
 * topology entries; the generic bring-up module validates, starts, and
 * waits for secondary harts. RISC-V SBI/HSM operations are the arch
 * adapter, never visible to init/main.c.
 */

/* Boot-error codes. Pure defines so boot.S may include this header. */
#define SMP_BOOT_ERR_NONE	     0
#define SMP_BOOT_ERR_HARTID_MISMATCH 1
#define SMP_BOOT_ERR_STATE_MISMATCH  2
#define SMP_BOOT_ERR_NO_SATP	     3
#define SMP_BOOT_ERR_HSM_START	     4

#ifndef __ASSEMBLER__

#include <nuvix/types.h>
#include <arch/smp.h>

struct cpu_topology_entry;

/*
 * Platform CPU-data module: the sole owner of hart enumeration. It maps the
 * SBI boot hart to logical CPU 0 and fills the caller-provided entries.
 */
int platform_cpu_entries(uint32_t boot_hartid,
			 struct cpu_topology_entry *entries, uint32_t *count);

/*
 * Generic bring-up. smp_prepare() validates the boot hart and topology;
 * smp_boot_cpus() starts secondaries and waits for ONLINE, panicking on any
 * mismatch or timeout. Both run on logical CPU 0, which is the SBI boot
 * hart for this boot.
 */
int smp_prepare(uint32_t boot_hartid);
void smp_boot_cpus(void);

/* Secondary entry from the assembly trampoline; never returns. */
__noreturn
void smp_secondary_main(uint32_t hartid, uint32_t logical_id);

/* Per-CPU boot-error slot, writable by the pre-satp trampoline. */
extern uint32_t smp_boot_errors[];

/* True once smp_boot_cpus() finished the mandatory boot gate.  Before it,
 * CPU 0 runs with IRQs disabled and cannot acknowledge shootdown IPIs, so
 * global-flush helpers must skip remote shootdown. */
bool smp_booted(void);

#endif /* !__ASSEMBLER__ */
#endif
