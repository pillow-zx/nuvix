/*
 * kernel/smp.c - generic CPU bring-up and secondary idle
 *
 * CPU 0 coordinates topology publication, HSM start, and the acquire wait
 * for each secondary's self-published ONLINE state. Secondary harts run
 * only their local idle loop: no scheduler, no allocator, no I/O.
 */

#include <kernel/smp.h>
#include <kernel/cpu.h>
#include <kernel/processor.h>
#include <kernel/task.h>
#include <kernel/timer.h>

/* Boot-error slot per CPU. Written by the pre-satp trampoline (plain store)
 * and by smp_secondary_main() before a PARKED release store; CPU 0 reads it
 * only for failure diagnostics. */
uint32_t smp_boot_errors[NR_CPUS];

__noreturn void smp_secondary_main(uint32_t hartid, uint32_t logical_id)
{
	(void)hartid;
	(void)logical_id;

	for (;;)
		wait_for_interrupt();
}
