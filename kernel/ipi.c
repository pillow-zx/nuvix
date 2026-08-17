/*
 * kernel/ipi.c - generic IPI reason protocol
 *
 * Protocol: sender release-ORs reasons then sends; receiver clears SSIP,
 * acquire-exchanges the pending bits, dispatches, and release-publishes an
 * observed flag. A stale interrupt with an empty reason set is legal.
 */

#include <nuvix/ipi.h>
#include <nuvix/atomic.h>
#include <nuvix/cpu.h>
#include <nuvix/errno.h>
#include <nuvix/sched.h>
#include <nuvix/timer.h>
#include <nuvix/printk.h>
#include <arch/smp.h>
#include <asm/csr.h>

/* Pending reasons per CPU; publication release, consumption acquire. */
static atomic_isize_t ipi_pending[NR_CPUS];
/* Boot-health: set once per CPU after a reason was handled. */
static atomic_isize_t ipi_seen_flags[NR_CPUS];
/* Shootdown acks: monotonic counter incremented on each shootdown IPI. */
static atomic_isize_t shootdown_acks[NR_CPUS];

int ipi_send(uint32_t cpu_id, int reasons)
{
	struct cpu *cpu;

	if (!reasons || (reasons & ~IPI_REASON_MASK))
		return -EINVAL;
	cpu = cpu_by_id(cpu_id);
	if (!cpu || cpu == current_cpu() || !cpu_is_online(cpu_id))
		return -EINVAL;
	atomic_isize_or_fetch_release(&ipi_pending[cpu_id], (isize)reasons);
	return smp_arch_ipi_notify(cpu->hartid);
}

void ipi_handle(void)
{
	struct cpu *cpu = current_cpu();
	isize reasons;

	/* Acknowledge first so a second send can interrupt again while this
	 * handler runs. */
	smp_arch_ipi_ack();
	reasons = atomic_isize_xchg_acquire(&ipi_pending[cpu->id], 0);
	if (!reasons)
		return;
	if (reasons & IPI_RESCHEDULE) {
		/* No-op for an idle current task; never schedules in IRQ
		 * context. The trap-return path consumes need_resched. */
		sched_request();
	}
	if (reasons & IPI_SHOOTDOWN)
		tlb_flush_all();
	if (reasons & IPI_FENCE_I)
		icache_flush();
	if (reasons & (IPI_SHOOTDOWN | IPI_FENCE_I))
		atomic_isize_add_release(&shootdown_acks[cpu->id], 1);
	/* Boot-health flag: set once per CPU after any reason was handled. */
	atomic_isize_set_release(&ipi_seen_flags[cpu->id], 1);
}

bool ipi_seen(uint32_t cpu_id)
{
	return atomic_isize_read_acquire(&ipi_seen_flags[cpu_id]) != 0;
}

int ipi_pending_reasons(uint32_t cpu_id)
{
	return (int)atomic_isize_read_acquire(&ipi_pending[cpu_id]);
}

void ipi_send_shootdown(uint32_t cpu_id, int reasons)
{
	isize before_ack, after_ack;
	uint64_t deadline;

	BUG_ON(!(reasons & (IPI_SHOOTDOWN | IPI_FENCE_I)));
	before_ack = atomic_isize_read_acquire(&shootdown_acks[cpu_id]);
	BUG_ON(ipi_send(cpu_id, reasons) != 0);

	deadline = timer_now() + MTIME_FREQ;
	while (1) {
		after_ack = atomic_isize_read_acquire(&shootdown_acks[cpu_id]);
		if (after_ack != before_ack)
			break;
		if (timer_now() >= deadline) {
			panic("shootdown timeout cpu=%u reasons=0x%x before=%ld after=%ld",
			      cpu_id, reasons, (long)before_ack, (long)after_ack);
		}
	}
}

