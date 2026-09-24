#ifndef _NUVIX_SMP_H
#define _NUVIX_SMP_H

/*
 * include/nuvix/smp.h - CPU bring-up coordination
 *
 * Secondary CPU preparation and startup. Common topology and boot CPU
 * initialization belong to cpu.h. RISC-V SBI/HSM operations stay in the
 * architecture adapter.
 */

/* Boot-error codes. Pure defines so boot.S may include this header. */
#define SMP_BOOT_ERR_NONE	     0
#define SMP_BOOT_ERR_HARTID_MISMATCH 1
#define SMP_BOOT_ERR_STATE_MISMATCH  2
#define SMP_BOOT_ERR_NO_SATP	     3
#define SMP_BOOT_ERR_START	     4

#ifndef __ASSEMBLER__

#include <nuvix/types.h>

#ifdef CONFIG_SMP
/*
 * Generic bring-up. smp_prepare() checks secondary CPU boot requirements
 * after cpu_prepare() has published the topology;
 * smp_boot_cpus() starts secondaries and waits for ONLINE, panicking on any
 * mismatch or timeout. Both run on logical CPU 0, which is the boot CPU.
 */
void smp_prepare(void);
void smp_boot_cpus(void);

/* Report a completed local scheduler tick to the secondary boot gate. */
void smp_timer_tick(void);

/* Secondary entry from the assembly trampoline; never returns. */
__noreturn
void smp_secondary_main(uint32_t hartid, uint32_t logical_id);

/* Per-CPU boot-error slot, writable by the pre-satp trampoline. */
extern uint32_t smp_boot_errors[];

/* True once smp_boot_cpus() finished the mandatory boot gate. Global-flush
 * helpers use this boundary to avoid targeting secondaries before every hart
 * has proved its local interrupt and timer state. */
bool smp_booted(void);
#else
static inline void smp_prepare(void)
{
}

static inline void smp_boot_cpus(void)
{
}

static inline void smp_timer_tick(void)
{
}

/* No remote CPUs ever need a shootdown in a uniprocessor build. */
static inline bool smp_booted(void)
{
	return false;
}
#endif

#endif /* !__ASSEMBLER__ */
#endif
