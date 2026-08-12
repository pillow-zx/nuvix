#ifndef _CUTEOS_KERNEL_CPU_H
#define _CUTEOS_KERNEL_CPU_H

/*
 * include/kernel/cpu.h - CPU-local kernel state
 */

#include <kernel/types.h>
#include <kernel/compiler.h>
#include <kernel/tools.h>
#include <kernel/printk.h>
#include <kernel/atomic.h>
#include <arch/cpu.h>
#include <arch/irq.h>

#define NR_CPUS CONFIG_QEMU_CPUS

#define CPU_OFFLINE  0u
#define CPU_BOOTING  1u
#define CPU_ONLINE   2u
#define CPU_PARKED   3u
#define CPU_LOCK_MAX 16u

struct task_struct;
struct spinlock;

/*
 * One enumerated CPU slot: a logical ID plus the platform hart it runs on.
 * The generic CPU module never infers logical ID == hartid; only the platform
 * topology input may state that equality.
 */
struct cpu_topology_entry {
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
	/* U->S trap entry parks user t0 and t1 here before switching to the
	 * kernel stack; must stay before the config-dependent lock array. */
	uintptr_t entry_scratch[2];
	IFDEF(CONFIG_DEBUG_CONTEXT, struct spinlock *locks[CPU_LOCK_MAX];
	      irq_flags_t lock_flags[CPU_LOCK_MAX];
	      bool lock_irqsave[CPU_LOCK_MAX];)
};

static_assert(offsetof(struct cpu, current_task) == CPU_CURRENT_TASK,
	      "CPU_CURRENT_TASK offset in entry.S out of sync with struct cpu");
static_assert(
	offsetof(struct cpu, preempt_count) == CPU_PREEMPT_COUNT,
	"CPU_PREEMPT_COUNT offset in entry.S out of sync with struct cpu");
static_assert(offsetof(struct cpu, entry_scratch) == CPU_ENTRY_SCRATCH,
	      "CPU_ENTRY_SCRATCH offset in entry.S out of sync with struct cpu");

extern struct cpu cpu_table[NR_CPUS];
extern uint32_t nr_cpu_ids;

/*
 * One-shot topology publication. Validates the entry count against NR_CPUS and
 * the entries themselves (unique logical IDs, unique hart IDs) before filling
 * cpu_table; nr_cpu_ids becomes immutable once it returns.
 */
int cpu_topology_init(const struct cpu_topology_entry *entries, uint32_t count);

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
__always_inline __nonnull(1)
static inline void cpu_state_store_release(struct cpu *cpu, uint32_t state)
{
	atomic_set_release(&cpu->state, (int)state);
}

__always_inline __must_check __pure __nonnull(1)
static inline uint32_t cpu_state_load_acquire(const struct cpu *cpu)
{
	return (uint32_t)atomic_read_acquire(&cpu->state);
}

/*
 * Online versus schedulable: the online mask records CPUs whose local state
 * exists; the schedulable mask records CPUs available to ordinary tasks.
 * Publication of both is release; observation is acquire.
 */
__must_check __pure
uint64_t cpu_online_mask(void);
__must_check __pure
uint64_t cpu_schedulable_mask(void);

void cpu_set_online(uint32_t id);
void cpu_set_schedulable(uint32_t id);

__always_inline __must_check __pure __returns_nonnull
static inline struct cpu *current_cpu(void)
{
	return arch_current_cpu();
}
__always_inline __must_check __pure
static inline struct cpu *cpu_by_id(uint32_t id)
{
	return id < nr_cpu_ids ? &cpu_table[id] : NULL;
}

__always_inline __must_check __pure
static inline bool cpu_is_online(uint32_t id)
{
	return id < nr_cpu_ids && (cpu_online_mask() & (1ULL << id));
}

__always_inline __must_check __pure
static inline bool cpu_is_schedulable(uint32_t id)
{
	return id < nr_cpu_ids && (cpu_schedulable_mask() & (1ULL << id));
}

__always_inline __must_check __pure __nonnull(1)
static inline struct task_struct *cpu_current_task(const struct cpu *cpu)
{
	return cpu->current_task;
}

__always_inline __nonnull(1)
static inline void cpu_set_task(struct cpu *cpu, struct task_struct *task)
{
	cpu->current_task = task;
}

__always_inline __must_check __pure
static inline struct task_struct *current_task(void)
{
	return cpu_current_task(current_cpu());
}

__always_inline
static inline void set_current_task(struct task_struct *task)
{
	cpu_set_task(current_cpu(), task);
}

__always_inline __must_check __pure __nonnull(1)
static inline struct task_struct *cpu_idle_task(const struct cpu *cpu)
{
	return cpu->idle_task;
}

__always_inline __must_check __pure __nonnull(1)
static inline int cpu_preempt_count(const struct cpu *cpu)
{
	return cpu->preempt_count;
}

__always_inline __nonnull(1)
static inline void cpu_set_preempt_count(struct cpu *cpu, int count)
{
	BUG_ON(count < 0);
	cpu->preempt_count = count;
}

__always_inline __nonnull(1)
static inline void cpu_inc_preempt_count(struct cpu *cpu)
{
	BUG_ON(cpu_preempt_count(cpu) == INT32_MAX);
	cpu->preempt_count++;
}

__always_inline __nonnull(1)
static inline void cpu_dec_preempt_count(struct cpu *cpu)
{
	BUG_ON(cpu_preempt_count(cpu) <= 0);
	cpu->preempt_count--;
}

__always_inline __must_check __pure __nonnull(1)
static inline uint32_t cpu_lock_depth(const struct cpu *cpu)
{
	return cpu->lock_depth;
}

__always_inline __must_check __pure
static inline uint32_t lock_depth(void)
{
	return cpu_lock_depth(current_cpu());
}

__always_inline __must_check __pure __nonnull(1)
static inline uint32_t cpu_irq_nesting(const struct cpu *cpu)
{
	return cpu->irq_nesting;
}

__always_inline __nonnull(1)
static inline void cpu_inc_irq_nesting(struct cpu *cpu)
{
	cpu->irq_nesting++;
}

__always_inline __nonnull(1)
static inline void cpu_dec_irq_nesting(struct cpu *cpu)
{
	cpu->irq_nesting--;
}

#endif
