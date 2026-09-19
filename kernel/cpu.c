/*
 * kernel/cpu.c - CPU topology, state, and CPU-local boot state
 */

#include <nuvix/cpu.h>
#include <nuvix/errno.h>
#include <nuvix/task.h>

struct cpu cpu_table[NR_CPUS];
uint32_t nr_cpu_ids;

/* CPU masks. Publication is release, observation is acquire; the scheduler
 * and syscall layers query only through cpu_online_mask()/cpu_schedulable_mask().
 * The QEMU virt target has no CPU hotplug: both masks are monotonic after boot,
 * so an online target remains able to consume an IPI selected from a snapshot. */
static atomic64_t online_cpu_mask;
static atomic64_t schedulable_cpu_mask;

int cpu_prepare(uint32_t boot_hartid)
{
	struct cpu_topology_entry entries[NR_CPUS];
	uint32_t count;
	int ret;

	ret = platform_cpu_entries(boot_hartid, entries, &count);
	if (ret < 0 || !count || count > NR_CPUS)
		return -EINVAL;
	return cpu_topology_init(entries, count);
}

void cpu_boot_online(void)
{
	BUG_ON(!nr_cpu_ids || current_cpu() != &cpu_table[0]);
	cpu_state_store_release(&cpu_table[0], CPU_ONLINE);
	cpu_set_online(0);
	cpu_set_schedulable(0);
}

uint64_t cpu_online_mask(void)
{
	return (uint64_t)atomic64_read_acquire(&online_cpu_mask);
}

uint64_t cpu_schedulable_mask(void)
{
	return (uint64_t)atomic64_read_acquire(&schedulable_cpu_mask);
}

void cpu_set_online(uint32_t id)
{
	atomic64_or_fetch_release(&online_cpu_mask, (int64_t)(1ULL << id));
}

void cpu_set_schedulable(uint32_t id)
{
	atomic64_or_fetch_release(&schedulable_cpu_mask, (int64_t)(1ULL << id));
}

int cpu_topology_init(const struct cpu_topology_entry *entries, uint32_t count)
{
	uint32_t i;
	uint32_t j;

	if (!entries || !count || count > NR_CPUS)
		return -EINVAL;
	/* Validate the whole set before touching any slot: unique logical IDs
	 * within array capacity and unique hart IDs. */
	for (i = 0; i < count; i++) {
		if (entries[i].logical_id >= NR_CPUS)
			return -EINVAL;
		for (j = 0; j < i; j++) {
			if (entries[j].logical_id == entries[i].logical_id ||
			    entries[j].hartid == entries[i].hartid)
				return -EINVAL;
		}
	}
	for (i = 0; i < count; i++) {
		struct cpu *cpu = &cpu_table[entries[i].logical_id];

		cpu->id = entries[i].logical_id;
		cpu->hartid = entries[i].hartid;
		atomic_init(&cpu->state, CPU_OFFLINE);
	}
	nr_cpu_ids = count;
	return 0;
}

void cpu_boot_init(struct task_struct *idles)
{
	BUG_ON(!idles);
	for (uint32_t id = 0; id < NR_CPUS; id++) {
		struct cpu *cpu = &cpu_table[id];

		cpu->flags = 0;
		cpu->idle_task = &idles[id];
		/* sched_init snapshots this before secondary harts start. Both
		 * CPU-local and runqueue current must name the same initial idle;
		 * online/schedulable publication separately controls admission. */
		cpu->current_task = &idles[id];
		cpu->preempt_count = 0;
		cpu->irq_nesting = 0;
		cpu->lock_depth = 0;
#ifdef CONFIG_DEBUG_CONTEXT
		for (uint32_t lock = 0; lock < CPU_LOCK_MAX; lock++) {
			cpu->locks[lock] = NULL;
			cpu->lock_flags[lock] = 0;
			cpu->lock_irqsave[lock] = false;
		}
#endif
	}
}
