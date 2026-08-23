/*
 * kernel/smp.c - generic CPU bring-up and secondary idle
 *
 * Logical CPU 0 coordinates topology publication, HSM start, and the acquire
 * wait for each secondary's self-published ONLINE state. Secondary harts run
 * only their local idle loop (no ordinary Task dispatch, no I/O); a bounded
 * boot-time allocator/console self-test is their only allocation.
 */

#include <nuvix/smp.h>
#include <nuvix/bootinfo.h>
#include <nuvix/cpu.h>
#include <nuvix/errno.h>
#include <nuvix/ipi.h>
#include <nuvix/pgtable.h>
#include <nuvix/processor.h>
#include <nuvix/printk.h>
#include <nuvix/sched.h>
#include <nuvix/slab.h>
#include <nuvix/vmalloc.h>
#include <nuvix/task.h>
#include <nuvix/timer.h>
#include <nuvix/irq.h>
#include <asm/csr.h>
#include <arch/trap.h>

/* Boot-error slot per CPU. Written by the pre-satp trampoline (plain store)
 * and by smp_secondary_main() before a PARKED release store; logical CPU 0
 * reads it only for failure diagnostics. */
uint32_t smp_boot_errors[NR_CPUS];

/* Set after the boot gate; acquire semantics pairs with the release in
 * smp_boot_cpus(). */
static atomic_t smp_boot_done;

bool smp_booted(void)
{
	return atomic_read_acquire(&smp_boot_done) != 0;
}

__noreturn
static void smp_secondary_park(void)
{
	for (;;)
		wait_for_interrupt();
}

__noreturn
static void smp_boot_fail(uint32_t id, const char *reason,
				     uint32_t state)
{
	struct cpu *cpu = &cpu_table[id];
	struct smp_hart_status status = smp_hart_status(cpu->hartid);
	const char *name = smp_hart_status_name((uint64_t)status.value);

	pr_err("smp: cpu %u (hart %u) boot failed: %s\n"
	       "smp:   cpu state=%u boot_error=%u\n"
	       "smp:   hsm status error=%ld value=%ld %s\n",
	       id, cpu->hartid, reason, state, smp_boot_errors[id],
	       status.error, status.value, name ? name : "unknown");
	panic("smp: cpu %u (hart %u) boot failed\n", id, cpu->hartid);
	unreachable();
}

__noreturn
static void smp_gate_fail(uint64_t secondary_mask,
				     uint64_t timer_seen, uint64_t ipi_observed,
				     const char *what)
{
	pr_err("smp: boot gate failed: %s\n"
	       "smp:   secondary expected=0x%lx online=0x%lx "
	       "schedulable=0x%lx\n"
	       "smp:   timer_seen=0x%lx ipi_seen=0x%lx\n",
	       what, secondary_mask, cpu_online_mask(),
	       cpu_schedulable_mask(), timer_seen, ipi_observed);
	for (uint32_t id = 0; id < nr_cpu_ids; id++) {
		struct cpu *cpu = &cpu_table[id];

		pr_err("smp:   cpu %u: hart=%u state=%u boot_error=%u "
		       "timer_seen=%d ipi_seen=%d pending=0x%x\n",
		       id, cpu->hartid, cpu_state_load_acquire(cpu),
		       smp_boot_errors[id], cpu_timer_seen(id), ipi_seen(id),
		       ipi_pending_reasons(id));
	}
	panic("smp: boot gate failed: %s\n", what);
	unreachable();
}

static void smp_probe_record(uint64_t timer_seen, uint64_t ipi_seen);

static void smp_boot_gate(uint32_t boot_id, uint64_t *timer_seen_out,
			  uint64_t *ipi_seen_out)
{
	uint64_t secondary_mask = 0;
	uint64_t timer_seen = 0;
	uint64_t ipi_observed = 0;
	uint64_t deadline;
	uint32_t id;
	uint32_t nr_cpus;

	BUG_ON(!timer_seen_out || !ipi_seen_out);
	nr_cpus = nr_cpu_ids;
	BUG_ON(!nr_cpus || nr_cpus > NR_CPUS || boot_id >= nr_cpus);

	for (id = 0; id < nr_cpus; id++)
		if (id != boot_id)
			secondary_mask |= (1ULL << id);
	BUG_ON((cpu_online_mask() & ~(1ULL << boot_id)) != secondary_mask);
	BUG_ON(cpu_schedulable_mask() != SCHED_BOOT_AFFINITY_MASK);

	if (nr_cpus == 1)
		goto out;

	deadline = timer_now() + MTIME_FREQ;
	while (timer_seen != secondary_mask) {
		timer_seen = 0;
		for (id = 0; id < nr_cpus; id++)
			if (id != boot_id && cpu_timer_seen(id))
				timer_seen |= (1ULL << id);
		if ((int64_t)(deadline - timer_now()) < 0)
			smp_gate_fail(secondary_mask, timer_seen,
				      ipi_observed, "timer-seen");
	}

	for (id = 0; id < nr_cpus; id++) {
		if (id == boot_id)
			continue;
		if (ipi_send(id, IPI_RESCHEDULE) != 0)
			smp_gate_fail(secondary_mask, timer_seen,
				      ipi_observed, "ipi-send");
	}

	deadline = timer_now() + MTIME_FREQ;
	while (ipi_observed != secondary_mask) {
		ipi_observed = 0;
		for (id = 0; id < nr_cpus; id++)
			if (id != boot_id && ipi_seen(id))
				ipi_observed |= (1ULL << id);
		if ((int64_t)(deadline - timer_now()) < 0)
			smp_gate_fail(secondary_mask, timer_seen,
				      ipi_observed, "ipi-seen");
	}

out:
	*timer_seen_out = timer_seen;
	*ipi_seen_out = ipi_observed;
}

static void smp_probe_record(uint64_t timer_seen, uint64_t ipi_seen)
{
	char harts[128];
	size_t off = 0;

	for (uint32_t id = 0; id < nr_cpu_ids; id++)
		off = bootinfo_append(harts, sizeof(harts), off, "%s%u",
				      off ? "," : "", cpu_table[id].hartid);

	printk_ring_record(LOG_INFO,
			   "SMP Probe: cpus=%u boot=%u online=0x%016llx "
			   "schedulable=0x%016llx timer_seen=0x%016llx "
			   "ipi_seen=0x%016llx harts=%s\n",
			   nr_cpu_ids, cpu_table[0].hartid,
			   (unsigned long long)cpu_online_mask(),
			   (unsigned long long)cpu_schedulable_mask(),
			   (unsigned long long)timer_seen,
			   (unsigned long long)ipi_seen, harts);
}

static void smp_wait_online(uint32_t id)
{
	struct cpu *cpu = &cpu_table[id];
	uint64_t deadline = timer_now() + MTIME_FREQ;
	uint32_t state;

	/* Acquire-wait for the secondary's own BOOTING -> ONLINE release
	 * publication, with a wrap-safe one-second mtime deadline. */
	while ((state = cpu_state_load_acquire(cpu)) == CPU_BOOTING) {
		if ((int64_t)(deadline - timer_now()) < 0)
			smp_boot_fail(id, "timeout", state);
	}
	if (state != CPU_ONLINE)
		smp_boot_fail(id, state == CPU_PARKED ? "parked" : "bad-state",
			      state);
	cpu_set_online(id);
}

int smp_prepare(uint32_t boot_hartid)
{
	struct cpu_topology_entry entries[NR_CPUS];
	uint32_t count;
	int ret;

	ret = platform_cpu_entries(boot_hartid, entries, &count);
	if (ret < 0 || !count || count > NR_CPUS)
		return -EINVAL;
	ret = cpu_topology_init(entries, count);
	if (ret < 0)
		return ret;
	/* Platform contract checks panic here; no reduced-CPU fallback exists. */
	smp_basic_prepare();
	return 0;
}

void smp_boot_cpus(void)
{
	uint32_t boot_id;
	uint32_t id;
	uint64_t timer_seen;
	uint64_t ipi_seen;

	BUG_ON(!nr_cpu_ids || nr_cpu_ids > NR_CPUS);
	/* Secondaries switch to the kernel page table, so it must be published. */
	BUG_ON(!pgtable_boot_token_valid());

	boot_id = 0;

	/* Boot CPU local state (idle/current) was prepared before this call. */
	cpu_state_store_release(&cpu_table[boot_id], CPU_ONLINE);
	cpu_set_online(boot_id);
	cpu_set_schedulable(boot_id);

	for (id = 0; id < nr_cpu_ids; id++) {
		struct cpu *cpu = &cpu_table[id];

		if (id == boot_id)
			continue;
		smp_boot_errors[id] = SMP_BOOT_ERR_NONE;
		cpu_state_store_release(cpu, CPU_BOOTING);
		if (smp_start_cpu(cpu->hartid,
				       smp_secondary_entry_pa(),
				       id) != 0) {
			smp_boot_errors[id] = SMP_BOOT_ERR_HSM_START;
			smp_boot_fail(id, "hsm-start", CPU_BOOTING);
		}
		smp_wait_online(id);
	}

	/* Mandatory gate: timer/IPI proof from every secondary before any
	 * syscall/VFS/device or thread initialization proceeds. */
	smp_boot_gate(boot_id, &timer_seen, &ipi_seen);
	/* Secondaries stay online but unschedulable until the migration stage. */
	BUG_ON(cpu_schedulable_mask() != SCHED_BOOT_AFFINITY_MASK);
	smp_probe_record(timer_seen, ipi_seen);
	atomic_set_release(&smp_boot_done, 1);
}

/* Banner is only meaningful after smp_boot_cpus() published the masks. */
BOOTINFO_BLOCK(cpu, void,

	char table[128];
	size_t off = 0;
	uint32_t schedulable_count = 0;

	for (uint32_t id = 0; id < nr_cpu_ids; id++) {
		off = bootinfo_append(table, sizeof(table), off, "%s%u->%u",
				      off ? " " : "", id, cpu_table[id].hartid);
		if (cpu_is_schedulable(id))
			schedulable_count++;
	}

	BROW("CPU Table", "%s", table);
	BROW("SMP", "%u harts online, %u schedulable", nr_cpu_ids,
	     schedulable_count);
)

/* Runs before ONLINE so the boot gate proves every secondary passed; the only
 * secondary execution of the allocator/console locks until migration. */
static void smp_alloc_self_test(uint32_t logical_id)
{
	void *objs[4];
	const size_t sizes[] = {16, 128, 1024, 4096};
	void *region;

	for (size_t i = 0; i < 4; i++) {
		objs[i] = kmalloc(sizes[i], ALLOC_NOWAIT);
		BUG_ON(!objs[i]);
		memset(objs[i], 0, sizes[i]);
	}
	for (size_t i = 0; i < 4; i++)
		kfree(objs[i]);

	/* Concurrent secondaries exercise the shared-table mapping path. */
	region = vmalloc(PAGE_SIZE, ALLOC_NOWAIT);
	BUG_ON(!region);
	memset(region, 0, PAGE_SIZE);
	vfree(region);

	pr_debug("smp: cpu %u allocator/console self-test ok\n", logical_id);
}

__noreturn
void smp_secondary_main(uint32_t hartid, uint32_t logical_id)
{
	struct cpu *cpu = cpu_by_id(logical_id);

	/* S-mode tp is the CPU pointer: install this hart's slot as the
	 * first statement. tp is a reserved RISC-V register that the
	 * compiler never allocates, and this volatile asm orders the write
	 * before any current_cpu() read in the code below. */
	asm volatile("mv tp, %0" :: "r"(cpu));

	if (!cpu || cpu->hartid != hartid) {
		if (cpu) {
			smp_boot_errors[logical_id] =
				SMP_BOOT_ERR_HARTID_MISMATCH;
			cpu_state_store_release(cpu, CPU_PARKED);
		}
		smp_secondary_park();
	}
	if (cpu_state_load_acquire(cpu) != CPU_BOOTING) {
		smp_boot_errors[logical_id] = SMP_BOOT_ERR_STATE_MISMATCH;
		cpu_state_store_release(cpu, CPU_PARKED);
		smp_secondary_park();
	}
	BUG_ON(current_cpu() != cpu);

	set_current_task(cpu_idle_task(cpu));
	trap_cpu_init();
	timer_cpu_init();
	clockevent_cpu_init();

	smp_alloc_self_test(logical_id);

	cpu_state_store_release(cpu, CPU_ONLINE);

	for (;;) {
		local_irq_enable();
		schedule();
		local_irq_enable();
		wait_for_interrupt();
	}
}
