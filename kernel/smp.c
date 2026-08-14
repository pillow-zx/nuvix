/*
 * kernel/smp.c - generic CPU bring-up and secondary idle
 *
 * Logical CPU 0 coordinates topology publication, HSM start, and the acquire
 * wait for each secondary's self-published ONLINE state. Secondary harts run
 * only their local idle loop: no scheduler, no allocator, no I/O.
 */

#include <nuvix/smp.h>
#include <nuvix/cpu.h>
#include <nuvix/errno.h>
#include <nuvix/ipi.h>
#include <nuvix/pgtable.h>
#include <nuvix/processor.h>
#include <nuvix/printk.h>
#include <nuvix/task.h>
#include <nuvix/timer.h>
#include <nuvix/irq.h>
#include <asm/csr.h>
#include <arch/trap.h>

/* Boot-error slot per CPU. Written by the pre-satp trampoline (plain store)
 * and by smp_secondary_main() before a PARKED release store; logical CPU 0
 * reads it only for failure diagnostics. */
uint32_t smp_boot_errors[NR_CPUS];

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
	struct smp_hart_status status = smp_arch_hart_status(cpu->hartid);
	const char *name = smp_arch_hart_status_name((uint64_t)status.value);

	pr_err("smp: cpu %u (hart %u) boot failed: %s\n"
	       "smp:   cpu state=%u boot_error=%u\n"
	       "smp:   hsm status error=%lld value=%lld %s\n",
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
	       "smp:   secondary expected=0x%llx online=0x%llx "
	       "schedulable=0x%llx\n"
	       "smp:   timer_seen=0x%llx ipi_seen=0x%llx\n",
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

static void smp_boot_gate(uint32_t boot_id)
{
	uint64_t secondary_mask = 0;
	uint64_t timer_seen = 0;
	uint64_t ipi_observed = 0;
	uint64_t deadline;
	uint32_t id;

	for (id = 0; id < nr_cpu_ids; id++)
		if (id != boot_id)
			secondary_mask |= (1ULL << id);
	BUG_ON((cpu_online_mask() & ~(1ULL << boot_id)) != secondary_mask);
	BUG_ON(cpu_schedulable_mask() != (1ULL << boot_id));

	if (nr_cpu_ids == 1)
		return; /* UP: validate state only; no SMP sentinel. */

	/* Every secondary must prove one local timer tick within one
	 * second; the observation is published after reprogramming. */
	deadline = timer_now() + MTIME_FREQ;
	while (timer_seen != secondary_mask) {
		timer_seen = 0;
		for (id = 0; id < nr_cpu_ids; id++)
			if (id != boot_id && cpu_timer_seen(id))
				timer_seen |= (1ULL << id);
		if ((int64_t)(deadline - timer_now()) < 0)
			smp_gate_fail(secondary_mask, timer_seen,
				      ipi_observed, "timer-seen");
	}

	/* One reschedule IPI per secondary; SBI failure is fatal. */
	for (id = 0; id < nr_cpu_ids; id++) {
		if (id == boot_id)
			continue;
		if (ipi_send(id, IPI_RESCHEDULE) != 0)
			smp_gate_fail(secondary_mask, timer_seen,
				      ipi_observed, "ipi-send");
	}

	/* Every secondary must acknowledge one delivery within one second. */
	deadline = timer_now() + MTIME_FREQ;
	while (ipi_observed != secondary_mask) {
		ipi_observed = 0;
		for (id = 0; id < nr_cpu_ids; id++)
			if (id != boot_id && ipi_seen(id))
				ipi_observed |= (1ULL << id);
		if ((int64_t)(deadline - timer_now()) < 0)
			smp_gate_fail(secondary_mask, timer_seen,
				      ipi_observed, "ipi-seen");
	}

	pr_info("[SMP] ready cpus=%u online=0x%llx schedulable=0x%llx "
		"timer_seen=0x%llx ipi_seen=0x%llx\n",
		nr_cpu_ids, cpu_online_mask(), cpu_schedulable_mask(),
		timer_seen, ipi_observed);
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
	pr_info("cpu: logical=%u hart=%u online\n", cpu->id, cpu->hartid);
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
	/* Platform contract checks (SBI version, probes, HSM states) are
	 * fatal and panic here; no reduced-CPU fallback exists. */
	smp_arch_prepare();
	return 0;
}

void smp_boot_cpus(void)
{
	uint32_t boot_id;
	uint32_t id;

	BUG_ON(nr_cpu_ids == 0);
	/* The kernel page table must be published and valid before any
	 * secondary can switch to it. */
	BUG_ON(!pgtable_boot_token_valid());

	/* platform_cpu_entries() normalizes the SBI boot hart to logical CPU 0. */
	boot_id = 0;

	/* Publish the boot CPU online/schedulable: its idle/current and local
	 * state were prepared before smp_boot_cpus() was called. */
	cpu_state_store_release(&cpu_table[boot_id], CPU_ONLINE);
	cpu_set_online(boot_id);
	cpu_set_schedulable(boot_id);
	pr_info("cpu: logical=%u hart=%u online\n", cpu_table[boot_id].id,
		cpu_table[boot_id].hartid);

	for (id = 0; id < nr_cpu_ids; id++) {
		struct cpu *cpu = &cpu_table[id];

		if (id == boot_id)
			continue;
		smp_boot_errors[id] = SMP_BOOT_ERR_NONE;
		cpu_state_store_release(cpu, CPU_BOOTING);
		if (smp_arch_start_cpu(cpu->hartid,
				       smp_arch_secondary_entry_pa(),
				       id) != 0) {
			smp_boot_errors[id] = SMP_BOOT_ERR_HSM_START;
			smp_boot_fail(id, "hsm-start", CPU_BOOTING);
		}
		smp_wait_online(id);
	}

	/* Mandatory boot gate: timer and IPI proof from every secondary,
	 * plus online/schedulable assertions, before any syscall/VFS/device
	 * or thread initialization proceeds. */
	smp_boot_gate(boot_id);
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

	/* Publish ONLINE only after all local state is complete. */
	cpu_state_store_release(cpu, CPU_ONLINE);

	/* Dedicated idle loop: timer interrupts keep flowing (sched_tick()
	 * no-ops for the idle task) but schedule() is never called here. */
	for (;;) {
		local_irq_enable();
		wait_for_interrupt();
	}
}
