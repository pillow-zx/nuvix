#include <nuvix/tlb.h>
#include <nuvix/cpu.h>
#include <nuvix/assert.h>

void flush_tlb_all(void)
{
    unsigned int local_cpu = current_cpu_id();

    local_flush_tlb_all();
    for (unsigned int cpu_id = 0; cpu_id < nr_cpus; ++cpu_id) {
        long ret;

        if (cpu_id == local_cpu)
            continue;
        ret = arch_remote_flush_tlb_all(cpu_id);
        Assert(!ret, "CPU%u: TLB flush failed (%d)\n", cpu_id, (int)ret);
    }
}

void flush_icache_all(void)
{
    unsigned int local_cpu = current_cpu_id();

    arch_local_flush_icache_all();
    for (unsigned int cpu_id = 0; cpu_id < nr_cpus; ++cpu_id) {
        long ret;

        if (cpu_id == local_cpu)
            continue;
        ret = arch_remote_flush_icache_all(cpu_id);
        Assert(!ret, "CPU%u: instruction cache flush failed (%d)\n", cpu_id,
               (int)ret);
    }
}
