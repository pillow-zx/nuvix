#ifndef _NUVIX_CPU_H
#define _NUVIX_CPU_H

/*
 * include/nuvix/cpu.h - CPU-local kernel state
 */

#include <nuvix/types.h>
#include <nuvix/bitops.h>
#include <nuvix/compiler.h>
#include <nuvix/tools.h>
#include <nuvix/printk.h>
#include <nuvix/atomic.h>
#include <nuvix/config.h>
#include <arch/cpu.h>

#define CPUMASK_WORD_BITS       64u
#define CPUMASK_WORDS           ((NR_CPUS + CPUMASK_WORD_BITS - 1) / CPUMASK_WORD_BITS)

typedef struct cpumask {
	uint64_t bits[CPUMASK_WORDS];
} cpumask_t;

__must_check __const
static inline size_t cpumask_size(void)
{
	return sizeof(cpumask_t);
}

__nonnull(1)
static inline void cpumask_zero(cpumask_t *mask)
{
	for (uint32_t word = 0; word < CPUMASK_WORDS; word++)
		mask->bits[word] = 0;
}

__nonnull(1, 2)
static inline void cpumask_copy(cpumask_t *to, const cpumask_t *from)
{
	for (uint32_t word = 0; word < CPUMASK_WORDS; word++)
		to->bits[word] = from->bits[word];
}

__nonnull(1)
static inline void cpumask_set_cpu(cpumask_t *mask, uint32_t cpu)
{
	if (cpu < NR_CPUS)
		mask->bits[cpu / CPUMASK_WORD_BITS] |=
			BIT_U64(cpu % CPUMASK_WORD_BITS);
}

__must_check __nonnull(1)
static inline bool cpumask_test_cpu(const cpumask_t *mask, uint32_t cpu)
{
	return cpu < NR_CPUS && (mask->bits[cpu / CPUMASK_WORD_BITS] &
		BIT_U64(cpu % CPUMASK_WORD_BITS));
}

__nonnull(1, 2, 3)
static inline void cpumask_and(cpumask_t *to, const cpumask_t *left, const cpumask_t *right)
{
	for (uint32_t word = 0; word < CPUMASK_WORDS; word++)
		to->bits[word] = left->bits[word] & right->bits[word];
}

__must_check __nonnull(1)
static inline bool cpumask_empty(const cpumask_t *mask)
{
	for (uint32_t word = 0; word < CPUMASK_WORDS; word++)
		if (mask->bits[word] != 0)
			return false;
	return true;
}

__must_check __nonnull(1)
static inline uint32_t cpumask_first(const cpumask_t *mask)
{
	for (uint32_t word = 0; word < CPUMASK_WORDS; word++) {
		if (mask->bits[word] != 0)
			return word * CPUMASK_WORD_BITS + ctzll(mask->bits[word]);
	}
	return NR_CPUS;
}

#define CPU_OFFLINE  0u
#define CPU_BOOTING  1u
#define CPU_ONLINE   2u
#define CPU_PARKED   3u

struct task_struct;

/*
 * One enumerated CPU slot: a logical ID plus the platform hart it runs on.
 * The generic CPU module never infers logical ID == hartid; only the platform
 * topology input may state that equality.
 */
struct cpu_entry {
	uint32_t logical_id;
	uint32_t hartid;
};

struct cpu {
	uint32_t id;
	uint32_t hartid;
	atomic_t state;
	uint32_t flags;
	struct task_struct *idle_task;
	struct task_struct *current_task;
	int preempt_count;
	uint32_t irq_nesting;
	uint32_t lock_depth;
	/* U->S trap entry parks user t0 and t1 here before switching stacks. */
	uintptr_t entry_scratch[2];
};

extern struct cpu cpu_table[NR_CPUS];
extern uint32_t nr_cpu_ids;

/* Platform enumeration maps the boot hart to logical CPU 0. */
int platform_cpu_entries(uint32_t boot_hartid,
			 struct cpu_entry *entries, uint32_t *count);

/* Publish topology before task, scheduler, and local interrupt setup. */
int cpu_prepare(uint32_t boot_hartid);

/* Publish logical CPU 0 after its idle task and local interrupts are ready. */
void cpu_boot_online(void);

/*
 * One-shot topology publication. Validates the entry count against NR_CPUS and
 * the entries themselves (unique logical IDs, unique hart IDs) before filling
 * cpu_table; nr_cpu_ids becomes immutable once it returns.
 */
int cpu_topology_init(const struct cpu_entry *entries, uint32_t count);

/*
 * Install per-CPU idle/current pointers and CPU-local counters. The topology
 * must already be initialized; each slot's state is managed through
 * cpu_state_store_release()/cpu_state_load_acquire() and the online mask.
 */
void cpu_boot_init(struct task_struct *idle_tasks);

/*
 * CPU state publication/observation. State transitions are release stores;
 * observation is acquire. Callers never read struct cpu.state directly.
 */
__nonnull(1)
static inline void cpu_state_store_release(struct cpu *cpu, uint32_t state)
{
	atomic_set_release(&cpu->state, (int)state);
}

__must_check __nonnull(1)
static inline uint32_t cpu_state_load_acquire(const struct cpu *cpu)
{
	return (uint32_t)atomic_read_acquire(&cpu->state);
}

/*
 * Online versus schedulable: the online mask records CPUs whose local state
 * exists; the schedulable mask records CPUs available to ordinary tasks.
 * Publication of both is release; observation is acquire. Neither is
 * __pure: the acquire reads must not be CSE'd across boot-gate iterations.
 */
__must_check
uint64_t cpu_online_mask(void);
__must_check
uint64_t cpu_schedulable_mask(void);

void cpu_set_online(uint32_t id);
void cpu_set_schedulable(uint32_t id);

__must_check __pure __returns_nonnull __hot
static inline struct cpu *current_cpu(void)
{
	return arch_current_cpu();
}

/* Install this CPU's kernel-local pointer during secondary bring-up. */
__always_inline
static inline void current_cpu_install(struct cpu *cpu)
{
	arch_current_cpu_install(cpu);
}

__must_check __pure
static inline struct cpu *cpu_by_id(uint32_t id)
{
	return id < nr_cpu_ids ? &cpu_table[id] : NULL;
}

__must_check __pure
static inline bool cpu_is_online(uint32_t id)
{
	return id < nr_cpu_ids && (cpu_online_mask() & (1ULL << id));
}

__must_check __pure
static inline bool cpu_is_schedulable(uint32_t id)
{
	return id < nr_cpu_ids && (cpu_schedulable_mask() & (1ULL << id));
}

__must_check __pure __nonnull(1) __hot
static inline struct task_struct *cpu_current_task(const struct cpu *cpu)
{
	return cpu->current_task;
}

__nonnull(1)
static inline void cpu_set_task(struct cpu *cpu, struct task_struct *task)
{
	cpu->current_task = task;
}

__must_check __pure __hot
static inline struct task_struct *current_task(void)
{
	return cpu_current_task(current_cpu());
}

static inline void set_current_task(struct task_struct *task)
{
	cpu_set_task(current_cpu(), task);
}

__must_check __pure __nonnull(1)
static inline struct task_struct *cpu_idle_task(const struct cpu *cpu)
{
	return cpu->idle_task;
}

__must_check __pure __nonnull(1)
static inline int cpu_preempt_count(const struct cpu *cpu)
{
	return cpu->preempt_count;
}

__nonnull(1)
static inline void cpu_set_preempt_count(struct cpu *cpu, int count)
{
	BUG_ON(count < 0);
	cpu->preempt_count = count;
}

__nonnull(1)
static inline void cpu_inc_preempt_count(struct cpu *cpu)
{
	BUG_ON(cpu_preempt_count(cpu) == INT32_MAX);
	cpu->preempt_count++;
}

__nonnull(1)
static inline void cpu_dec_preempt_count(struct cpu *cpu)
{
	BUG_ON(cpu_preempt_count(cpu) <= 0);
	cpu->preempt_count--;
}

__must_check __pure __nonnull(1)
static inline uint32_t cpu_lock_depth(const struct cpu *cpu)
{
	return cpu->lock_depth;
}

__must_check __pure
static inline uint32_t lock_depth(void)
{
	return cpu_lock_depth(current_cpu());
}

__must_check __pure __nonnull(1)
static inline uint32_t cpu_irq_nesting(const struct cpu *cpu)
{
	return cpu->irq_nesting;
}

__nonnull(1)
static inline void cpu_inc_irq_nesting(struct cpu *cpu)
{
	cpu->irq_nesting++;
}

__nonnull(1)
static inline void cpu_dec_irq_nesting(struct cpu *cpu)
{
	cpu->irq_nesting--;
}

#endif
