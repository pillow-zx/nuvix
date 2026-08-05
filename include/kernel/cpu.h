#ifndef _CUTEOS_KERNEL_CPU_H
#define _CUTEOS_KERNEL_CPU_H

/*
 * include/kernel/cpu.h - CPU-local kernel state
 */

#include <kernel/types.h>
#include <kernel/compiler.h>
#include <kernel/tools.h>
#include <kernel/printk.h>
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

struct cpu {
	uint32_t id;
	uint32_t hartid;
	uint32_t state;
	uint32_t flags;
	struct task_struct *idle_task;
	struct task_struct *current_task;
	int preempt_count;
	uint32_t irq_nesting;
	uint32_t lock_depth;
	IFDEF(CONFIG_DEBUG_CONTEXT, struct spinlock *locks[CPU_LOCK_MAX];
	      irq_flags_t lock_flags[CPU_LOCK_MAX];
	      bool lock_irqsave[CPU_LOCK_MAX];)
};

static_assert(offsetof(struct cpu, current_task) == CPU_CURRENT_TASK,
	      "CPU_CURRENT_TASK offset in entry.S out of sync with struct cpu");
static_assert(
	offsetof(struct cpu, preempt_count) == CPU_PREEMPT_COUNT,
	"CPU_PREEMPT_COUNT offset in entry.S out of sync with struct cpu");

extern struct cpu cpu_table[NR_CPUS];
extern uint32_t nr_cpu_ids;

void cpu_boot_init(struct task_struct *idle);

__always_inline __must_check __pure __returns_nonnull
static inline struct cpu *current_cpu(void)
{
	return arch_current_cpu(cpu_table);
}
__always_inline __must_check __pure
static inline struct cpu *cpu_by_id(uint32_t id)
{
	return id < NR_CPUS ? &cpu_table[id] : NULL;
}

__always_inline __must_check __pure
static inline bool cpu_is_online(uint32_t id)
{
	struct cpu *cpu = cpu_by_id(id);

	return cpu && cpu->state == CPU_ONLINE;
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
