/*
 * kernel/smp/ipi.c - generic reschedule IPI
 *
 * Scheduler state carries the work. The IPI only prompts the target CPU to
 * observe need_resched, so duplicate or coalesced notifications are harmless.
 */

#include <nuvix/ipi.h>
#include <nuvix/atomic.h>
#include <nuvix/cpu.h>
#include <nuvix/errno.h>
#include <nuvix/sched.h>
#include <arch/smp.h>

/* Boot-health: set once per CPU after an IPI was handled. */
static atomic_t ipi_seen_flags[NR_CPUS];

int ipi_send(uint32_t cpu_id)
{
	struct cpu *cpu;

	cpu = cpu_by_id(cpu_id);
	if (!cpu || !cpu_is_online(cpu_id))
		return -EINVAL;
	if (cpu == current_cpu()) {
		sched_request();
		return 0;
	}
	return smp_ipi_notify(cpu->hartid);
}

void ipi_handle(void)
{
	const struct cpu *cpu = current_cpu();
	uint32_t cpu_id = cpu->id;

	smp_ipi_ack();
	sched_request();
	atomic_set_release(&ipi_seen_flags[cpu_id], 1);
}

bool ipi_seen(uint32_t cpu_id)
{
	return atomic_read_acquire(&ipi_seen_flags[cpu_id]) != 0;
}
