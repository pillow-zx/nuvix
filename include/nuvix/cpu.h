#ifndef _NUVIX_CPU_H
#define _NUVIX_CPU_H

#include <nuvix/atomic.h>
#include <asm/cpu.h>

enum cpu_state {
    CPU_PREPARED,
    CPU_STARTING,
    CPU_READY,
    CPU_FAILED,
};

struct cpu {
    unsigned long hart_id;
    enum cpu_state state;
    atomic_t ipi_pending;
} __aligned(SMP_CACHE_BYTES);

/* Logical CPU IDs index [0, nr_cpus); hart IDs are fixed before SMP startup. */
extern struct cpu cpus[CONFIG_NR_CPUS];
extern unsigned int nr_cpus;

struct dtb;

/* Boot-only, once after dtb_init(), before starting secondary harts. */
int cpu_init(const struct dtb *dtb, unsigned long boot_hart);

static inline enum cpu_state cpu_state_read(const struct cpu *cpu)
{
    return __atomic_load_n(&cpu->state, __ATOMIC_ACQUIRE);
}

static inline void cpu_state_set(struct cpu *cpu, enum cpu_state state)
{
    __atomic_store_n(&cpu->state, state, __ATOMIC_RELEASE);
}

#define halt() arch_halt()

#define send_ipi(cpu_id) arch_send_ipi(cpu_id)

#endif
