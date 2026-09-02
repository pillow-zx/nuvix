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
#include <nuvix/rseq.h>
#include <nuvix/sched.h>
#include <nuvix/timer.h>
#include <nuvix/printk.h>
#include <arch/barrier.h>
#include <arch/smp.h>
#include <asm/csr.h>

enum ipi_sync_family {
	IPI_SYNC_SHOOTDOWN,
	IPI_SYNC_MEMBARRIER,
	IPI_SYNC_CORE_FENCE,
	IPI_SYNC_FAMILY_COUNT,
};

/* Pending reasons per CPU; publication release, consumption acquire. */
static atomic_isize_t ipi_pending[NR_CPUS];
/* Boot-health: set once per CPU after a reason was handled. */
static atomic_isize_t ipi_seen_flags[NR_CPUS];
/* Each synchronous family uses request and completion sequences.  A receiver
 * snapshots the request sequence before executing the operation, then
 * release-publishes exactly that sequence after completion.  A later request
 * therefore cannot be acknowledged by an earlier operation. */
static atomic_t ipi_sync_requests[IPI_SYNC_FAMILY_COUNT][NR_CPUS];
static atomic_t ipi_sync_completions[IPI_SYNC_FAMILY_COUNT][NR_CPUS];

static uint32_t ipi_sync_family_mask(int reasons)
{
	uint32_t families = 0;

	if (reasons & (IPI_SHOOTDOWN | IPI_FENCE_I))
		families |= BIT(IPI_SYNC_SHOOTDOWN);
	if (reasons & (IPI_MEMBARRIER | IPI_RSEQ))
		families |= BIT(IPI_SYNC_MEMBARRIER);
	if (reasons & IPI_SYNC_CORE)
		families |= BIT(IPI_SYNC_CORE_FENCE);
	return families;
}

int ipi_send(uint32_t cpu_id, int reasons)
{
	struct cpu *cpu;

	if (!reasons || (reasons & ~IPI_REASON_MASK))
		return -EINVAL;
	cpu = cpu_by_id(cpu_id);
	/* Online membership is monotonic because this target has no CPU hotplug.
	 * Once selected, a target cannot disappear before consuming the reason. */
	if (!cpu || cpu == current_cpu() || !cpu_is_online(cpu_id))
		return -EINVAL;
	atomic_isize_or_fetch_release(&ipi_pending[cpu_id], (isize)reasons);
	return smp_ipi_notify(cpu->hartid);
}

void ipi_handle(void)
{
	struct cpu *cpu = current_cpu();
	int32_t served[IPI_SYNC_FAMILY_COUNT] = {0};
	uint32_t families;
	isize reasons;

	/* Acknowledge first so a second send can interrupt again while this
	 * handler runs. */
	smp_ipi_ack();
	reasons = atomic_isize_xchg_acquire(&ipi_pending[cpu->id], 0);
	if (!reasons)
		return;
	families = ipi_sync_family_mask((int)reasons);
	for (uint32_t family = 0; family < IPI_SYNC_FAMILY_COUNT; family++)
		if (families & BIT(family))
			served[family] = atomic_read_acquire(
				&ipi_sync_requests[family][cpu->id]);
	if (reasons & IPI_RESCHEDULE) {
		/* No-op for an idle current task; never schedules in IRQ
		 * context. The trap-return path consumes need_resched. */
		sched_request();
	}
	if (reasons & IPI_SHOOTDOWN)
		tlb_flush_all();
	if (reasons & IPI_FENCE_I)
		icache_flush();
	if (reasons & (IPI_MEMBARRIER | IPI_SYNC_CORE | IPI_RSEQ))
		arch_mb();
	if (reasons & IPI_SYNC_CORE)
		icache_flush();
	if (reasons & IPI_RSEQ)
		rseq_request_restart(current_task(), RSEQ_EVENT_FORCE);
	for (uint32_t family = 0; family < IPI_SYNC_FAMILY_COUNT; family++)
		if (families & BIT(family))
			atomic_set_release(
				&ipi_sync_completions[family][cpu->id],
				served[family]);
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

void ipi_send_sync(uint32_t cpu_id, int reasons)
{
	int32_t tickets[IPI_SYNC_FAMILY_COUNT] = {0};
	uint32_t families = ipi_sync_family_mask(reasons);
	uint64_t deadline;

	BUG_ON(!families || (reasons & ~IPI_REASON_MASK));
	for (uint32_t family = 0; family < IPI_SYNC_FAMILY_COUNT; family++)
		if (families & BIT(family))
			tickets[family] = atomic_add_fetch_release(
				&ipi_sync_requests[family][cpu_id], 1);
	BUG_ON(ipi_send(cpu_id, reasons) != 0);

	deadline = timer_now() + MTIME_FREQ;
	while (1) {
		bool complete = true;

		for (uint32_t family = 0; family < IPI_SYNC_FAMILY_COUNT;
		     family++) {
			int32_t completed;

			if (!(families & BIT(family)))
				continue;
			completed = atomic_read_acquire(
				&ipi_sync_completions[family][cpu_id]);
			if ((int32_t)(completed - tickets[family]) < 0) {
				complete = false;
				break;
			}
		}
		if (complete)
			break;
		if ((int64_t)(deadline - timer_now()) < 0)
			panic("synchronous IPI timeout cpu=%u reasons=0x%x",
			      cpu_id, reasons);
	}
}
