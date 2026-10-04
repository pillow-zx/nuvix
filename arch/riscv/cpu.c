#include <nuvix/cpu.h>
#include <asm/sbi.h>

long arch_send_ipi(unsigned int cpu_id)
{
    return sbi_send_ipi(1UL, cpus[cpu_id].hart_id).error;
}
