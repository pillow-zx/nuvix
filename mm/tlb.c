#include <nuvix/tlb.h>
#include <nuvix/cpu.h>
#include <nuvix/assert.h>

void flush_tlb_all(void)
{
    local_flush_tlb_all();
#ifdef CONFIG_SMP
    unsigned int local_cpu = current_cpu_id();

    for (unsigned int cpu_id = 0; cpu_id < nr_cpus; ++cpu_id) {
        long ret;

        if (cpu_id == local_cpu)
            continue;
        ret = arch_remote_flush_tlb_all(cpu_id);
        Assert(!ret, "CPU%u: TLB flush failed (%d)\n", cpu_id, (int)ret);
    }
#endif
}

void flush_icache_all(void)
{
    arch_local_flush_icache_all();
#ifdef CONFIG_SMP
    unsigned int local_cpu = current_cpu_id();

    for (unsigned int cpu_id = 0; cpu_id < nr_cpus; ++cpu_id) {
        long ret;

        if (cpu_id == local_cpu)
            continue;
        ret = arch_remote_flush_icache_all(cpu_id);
        Assert(!ret, "CPU%u: instruction cache flush failed (%d)\n", cpu_id,
               (int)ret);
    }
#endif
}
