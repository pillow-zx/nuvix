/*
 * kernel/smp/boot.c - generic secondary CPU bring-up and idle
 *
 * Logical CPU 0 coordinates CPU start and the acquire
 * wait for each secondary's self-published ONLINE state. Secondary harts stay
 * unschedulable through the boot gate, then enter the ordinary scheduler once
 * every configured hart has proved local timer and IPI readiness.
 */

#include <nuvix/smp.h>
#include <nuvix/cpu.h>
#include <nuvix/ipi.h>
#include <nuvix/processor.h>
#include <nuvix/printk.h>
#include <nuvix/sched.h>
#include <nuvix/task.h>
#include <nuvix/timer.h>
#include <nuvix/irq.h>
#include <nuvix/trap.h>

/* Boot-error slot per CPU. Written by the pre-satp trampoline (plain store)
 * and by smp_secondary_main() before a PARKED release store; logical CPU 0
 * reads it only for failure diagnostics. */
uint32_t smp_boot_errors[NR_CPUS];

/* Set after the boot gate; acquire semantics pairs with the release in
 * smp_boot_cpus(). */
static atomic_t smp_boot_done;

/* Published after a CPU has handled and rearmed its first scheduler tick. */
static atomic64_t smp_timer_seen[NR_CPUS];

void smp_timer_tick(void)
{
	atomic64_set_release(&smp_timer_seen[current_cpu()->id], 1);
}

static bool cpu_timer_seen(uint32_t id)
{
	return atomic64_read_acquire(&smp_timer_seen[id]) != 0;
}

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

__noreturn __cold
static void smp_boot_fail(uint32_t id, const char *reason,
				     uint32_t state)
{
	struct cpu *cpu = &cpu_table[id];

	pr_err("smp: cpu %u boot failed: %s state=%u boot_error=%u\n",
	       id, reason, state, smp_boot_errors[id]);
	arch_cpu_start_diagnose(id);
	panic("smp: cpu %u (hart %u) boot failed\n", id, cpu->hartid);
	unreachable();
}

__noreturn __cold
static void smp_gate_fail(uint64_t secondary_mask, uint64_t timer_seen, uint64_t ipi_observed,
				     const char *what)
{
	pr_err("smp: boot gate failed: %s\n"
	       "smp:   secondary expected=0x%lx online=0x%lx "
	       "schedulable=0x%lx\n"
	       "smp:   timer_seen=0x%lx ipi_seen=0x%lx\n",
	       what, secondary_mask, cpu_online_mask(), cpu_schedulable_mask(),
	       timer_seen, ipi_observed);
	for (uint32_t id = 0; id < nr_cpu_ids; id++) {
		struct cpu *cpu = &cpu_table[id];

		pr_err("smp:   cpu %u: hart=%u state=%u boot_error=%u "
		       "timer_seen=%d ipi_seen=%d\n",
		       id, cpu->hartid, cpu_state_load_acquire(cpu),
		       smp_boot_errors[id], cpu_timer_seen(id), ipi_seen(id));
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
	uint32_t nr_cpus;

	nr_cpus = nr_cpu_ids;
	BUG_ON(!nr_cpus || nr_cpus > NR_CPUS || boot_id >= nr_cpus);

	for (id = 0; id < nr_cpus; id++)
		if (id != boot_id)
			secondary_mask |= (1ULL << id);
	BUG_ON((cpu_online_mask() & ~(1ULL << boot_id)) != secondary_mask);
	BUG_ON(cpu_schedulable_mask() != (1ULL << SCHED_BOOT_AFFINITY_CPU));

	if (nr_cpus == 1)
		return;

	deadline = timer_now() + timer_frequency;
	while (timer_seen != secondary_mask) {
		timer_seen = 0;
		for (id = 0; id < nr_cpus; id++)
			if (id != boot_id && cpu_timer_seen(id))
				timer_seen |= (1ULL << id);
		if ((int64_t)(deadline - timer_now()) < 0)
			smp_gate_fail(secondary_mask, timer_seen, ipi_observed,
				      "timer-seen");
	}

	for (id = 0; id < nr_cpus; id++) {
		if (id == boot_id)
			continue;
		if (ipi_send(id) != 0)
			smp_gate_fail(secondary_mask, timer_seen, ipi_observed,
				      "ipi-send");
	}

	deadline = timer_now() + timer_frequency;
	while (ipi_observed != secondary_mask) {
		ipi_observed = 0;
		for (id = 0; id < nr_cpus; id++)
			if (id != boot_id && ipi_seen(id))
				ipi_observed |= (1ULL << id);
		if ((int64_t)(deadline - timer_now()) < 0)
			smp_gate_fail(secondary_mask, timer_seen, ipi_observed,
				      "ipi-seen");
	}

}

static void smp_wait_online(uint32_t id)
{
	struct cpu *cpu = &cpu_table[id];
	uint64_t deadline = timer_now() + timer_frequency;
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

void smp_prepare(void)
{
	/* Platform contract checks panic here; no reduced-CPU fallback exists.
	 */
	arch_smp_prepare();
}

void smp_boot_cpus(void)
{
	uint32_t boot_id;
	uint32_t id;

	BUG_ON(!nr_cpu_ids || nr_cpu_ids > NR_CPUS);

	boot_id = 0;

	/* Common CPU initialization has already published the boot CPU. */
	BUG_ON(!cpu_is_online(boot_id) || !cpu_is_schedulable(boot_id));

	for (id = 0; id < nr_cpu_ids; id++) {
		struct cpu *cpu = &cpu_table[id];

		if (id == boot_id)
			continue;
		smp_boot_errors[id] = SMP_BOOT_ERR_NONE;
		cpu_state_store_release(cpu, CPU_BOOTING);
		if (arch_cpu_start(id) != 0) {
			smp_boot_errors[id] = SMP_BOOT_ERR_START;
			smp_boot_fail(id, "cpu-start", CPU_BOOTING);
		}
		smp_wait_online(id);
	}

	/* Mandatory gate: timer/IPI proof from every secondary before any
	 * syscall/VFS/device or thread initialization proceeds. */
	smp_boot_gate(boot_id);
	/* The boot gate is the publication boundary for ordinary Task SMP.  A
	 * configured secondary cannot receive placement before it proved local
	 * timer and IPI readiness; afterwards every online CPU is schedulable.
	 */
	for (id = 0; id < nr_cpu_ids; id++)
		if (cpu_is_online(id))
			cpu_set_schedulable(id);
	BUG_ON(cpu_schedulable_mask() != cpu_online_mask());
	atomic_set_release(&smp_boot_done, 1);
}

__noreturn
void smp_secondary_main(uint32_t hartid, uint32_t logical_id)
{
	struct cpu *cpu = cpu_by_id(logical_id);

	/* S-mode tp is the CPU pointer: install this hart's slot as the
	 * first statement, before any current_cpu() read below. */
	current_cpu_install(cpu);

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

	cpu_state_store_release(cpu, CPU_ONLINE);

	for (;;) {
		local_irq_enable();
		schedule();
		sched_idle();
	}
}
