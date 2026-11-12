#define pr_fmt(fmt) "cpu: " fmt

#include <nuvix/cpu.h>
#include <nuvix/module.h>
#include <nuvix/dtb.h>
#include <nuvix/errno.h>
#include <nuvix/printk.h>
#include <nuvix/task.h>
#include <nuvix/sched.h>
#include <nuvix/tick.h>

struct cpu cpus[CONFIG_NR_CPUS];
unsigned int nr_cpus;

static int register_cpu(unsigned long hart_id, void *arg)
{
    bool *found_boot_cpu = arg;

    if (hart_id == cpus[0].hart_id) {
        if (*found_boot_cpu) {
            pr_err("duplicate boot hart ID %p\n", (void *)hart_id);
            return -EEXIST;
        }
        *found_boot_cpu = true;
        return 0;
    }
#ifdef CONFIG_SMP
    for (unsigned int i = 1; i < nr_cpus; ++i) {
        if (cpus[i].hart_id == hart_id) {
            pr_err("duplicate hart ID %p\n", (void *)hart_id);
            return -EEXIST;
        }
    }
    if (nr_cpus == CONFIG_NR_CPUS) {
        pr_warn("hart %p skipped: CPU capacity %u reached\n", (void *)hart_id,
                (unsigned int)CONFIG_NR_CPUS);
        return 0;
    }
    cpus[nr_cpus++].hart_id = hart_id;
#endif
    return 0;
}

int cpu_init(unsigned long boot_hart)
{
    bool found_boot_cpu = false;
    int ret;

    cpus[0].hart_id = boot_hart;
    nr_cpus = 1;
    ret = dtb_scan_cpus(register_cpu, &found_boot_cpu);
    if (ret)
        return ret;
    if (!found_boot_cpu) {
        pr_err("boot hart %p missing from available CPUs\n", (void *)boot_hart);
        return -ENODEV;
    }
    for (unsigned int i = 0; i < nr_cpus; ++i)
        pr_info("CPU%u -> hart %p\n", i, (void *)cpus[i].hart_id);
    pr_info("%u CPU(s) selected\n", nr_cpus);
    return 0;
}

int cpu_local_init(void)
{
    int ret;

    current->on_cpu = true;
    ret = tick_cpu_init();
    if (ret)
        return ret;
    arch_cpu_init();
    return 0;
}

/* After allocator and tick setup, IRQ off, on the boot task's initial depth. */
static int cpu_startup(void)
{
    sched_init();
    sched_init_idle(0, &boot_task);
    boot_task.on_cpu = true;
#ifdef CONFIG_SMP
    return smp_init();
#else
    return cpu_local_init();
#endif
}
MODULE_INIT(cpu_startup, 90);
