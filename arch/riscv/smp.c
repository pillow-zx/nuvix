#include <nuvix/assert.h>
#include <nuvix/bitmap.h>
#include <nuvix/cpu.h>
#include <nuvix/task.h>
#include <nuvix/layout.h>
#include <nuvix/math.h>
#include <nuvix/page.h>
#include <asm/riscv/smp.h>
#include <asm/riscv/sbi.h>
#include <asm/riscv/tlb.h>

extern char smp_entry[];

static struct cpu_boot_data boot_data[CONFIG_NR_CPUS];

long arch_send_ipi(unsigned int cpu_id)
{
    __asm__ volatile("fence rw, rw" ::: "memory");
    return sbi_send_ipi(1UL, cpus[cpu_id].hart_id).error;
}

static void remote_flush(bool icache, vaddr_t start, size_t size)
{
    unsigned long base = ~0UL;
    unsigned int local_cpu = current_cpu_id();
    bool more;

    if (nr_cpus < 2)
        return;
    for (unsigned int i = 0; i < nr_cpus; ++i)
        if (i != local_cpu)
            base = MIN(base, cpus[i].hart_id);

    __asm__ volatile("fence rw, rw" ::: "memory");
    do {
        unsigned long mask = 0;
        unsigned long next = ~0UL;
        long ret;

        more = false;
        for (unsigned int i = 0; i < nr_cpus; ++i) {
            unsigned long hart = cpus[i].hart_id;

            if (i == local_cpu || hart < base)
                continue;
            if (hart - base < BITS_PER_LONG) {
                mask |= 1UL << (hart - base);
            } else {
                next = MIN(next, hart);
                more = true;
            }
        }
        if (base == ~0UL) {
            --base;
            mask <<= 1;
        }
        ret = icache ? sbi_remote_fence_i(mask, base).error
                     : sbi_remote_sfence_vma(mask, base, start, size).error;
        Assert(!ret, "SBI %s flush failed (base=%#lx mask=%#lx error=%ld)\n",
               icache ? "I-cache" : "TLB", base, mask, ret);
        base = next;
    } while (more);
}

void arch_remote_flush_tlb_all(void)
{
    remote_flush(false, 0, 0);
}

void arch_remote_flush_tlb_page(vaddr_t addr)
{
    remote_flush(false, addr, PAGE_SIZE);
}

void arch_remote_flush_icache_all(void)
{
    remote_flush(true, 0, 0);
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
