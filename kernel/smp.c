/*
 * kernel/smp.c - generic CPU bring-up and secondary idle
 *
 * CPU 0 coordinates topology publication, HSM start, and the acquire wait
 * for each secondary's self-published ONLINE state. Secondary harts run
 * only their local idle loop: no scheduler, no allocator, no I/O.
 */

#include <kernel/smp.h>
#include <kernel/cpu.h>
#include <kernel/errno.h>
#include <kernel/pgtable.h>
#include <kernel/processor.h>
#include <kernel/printk.h>
#include <kernel/task.h>
#include <kernel/timer.h>
#include <kernel/irq.h>
#include <arch/trap.h>

/* Boot-error slot per CPU. Written by the pre-satp trampoline (plain store)
 * and by smp_secondary_main() before a PARKED release store; CPU 0 reads it
 * only for failure diagnostics. */
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
	const struct cpu_topology_entry *entries;
	uint32_t count;
	int ret;

	entries = platform_cpu_entries(&count);
	if (!entries || !count || count > NR_CPUS)
		return -EINVAL;
	if (boot_hartid != platform_boot_hartid())
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

	/* Find the boot CPU in the topology; never assume logical 0. */
	boot_id = 0;
	for (id = 0; id < nr_cpu_ids; id++)
		if (cpu_table[id].hartid == platform_boot_hartid())
			boot_id = id;
	BUG_ON(cpu_table[boot_id].hartid != platform_boot_hartid());

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

	/* Every configured CPU online and only the boot CPU schedulable,
	 * before any syscall/VFS/device/thread initialization proceeds. */
	for (id = 0; id < nr_cpu_ids; id++)
		BUG_ON(!cpu_is_online(id));
	BUG_ON(cpu_schedulable_mask() != (1ULL << boot_id));
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
