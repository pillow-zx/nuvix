#include <nuvix/cpu.h>
#include <nuvix/task.h>
#include <nuvix/layout.h>
#include <asm/smp.h>
#include <asm/sbi.h>

extern char smp_entry[], trap_entry[];

static struct cpu_boot_data boot_data[CONFIG_NR_CPUS];

long arch_send_ipi(unsigned int cpu_id)
{
    __asm__ volatile("fence rw, rw" ::: "memory");
    return sbi_send_ipi(1UL, cpus[cpu_id].hart_id).error;
}

long arch_start_cpu(unsigned int cpu_id, struct task *idle)
{
    struct cpu_boot_data *data = &boot_data[cpu_id];

    data->task = idle;
    data->stack = (unsigned long)idle->kstack + KSTACK_SIZE;
    data->satp = csr_read(satp);
    __asm__ volatile("fence rw, rw" ::: "memory");
    return sbi_hart_start(cpus[cpu_id].hart_id, __pa((vaddr_t)smp_entry),
                          __pa((vaddr_t)data))
        .error;
}

void arch_cpu_init(void)
{
    arch_local_irq_disable();
    csr_clear(sstatus, SSTATUS_FS);
    csr_write(sscratch, 0);
    arch_clear_ipi();
    csr_write(stvec, (unsigned long)trap_entry);
    csr_write(sie, SIE_SSIE | SIE_STIE);
    /* only allow cpu 0 to handle external irq */
    if (current_cpu_id() == 0)
        csr_set(sie, SIE_SEIE);
}
