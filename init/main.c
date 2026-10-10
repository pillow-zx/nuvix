#include <nuvix/ctypes.h>
#include <nuvix/compiler.h>
#include <nuvix/macro.h>
#include <nuvix/printk.h>
#include <nuvix/dtb.h>
#include <nuvix/cpu.h>
#include <nuvix/memblock.h>
#include <nuvix/page.h>
#include <nuvix/assert.h>
#include <nuvix/pgtable.h>
#include <nuvix/slab.h>
#include <nuvix/tick.h>
#include <nuvix/sched.h>
#include <nuvix/timer.h>
#include <nuvix/kthread.h>
#include <nuvix/irq.h>
#include <nuvix/device.h>

/* Called by the architecture's boot assembly. */
void kernel_main(unsigned long hart, paddr_t dtb_pa) __noreturn;

void kernel_main(unsigned long hart, paddr_t dtb_pa)
{
    struct dtb dtb;
    int ret;

#define BOOTINFO(ret, name)                                                    \
    ({                                                                         \
        if (ret) {                                                             \
            pr_err("%s: initialization failed (%d)\n", #name, ret);            \
            goto halt;                                                         \
        }                                                                      \
        pr_info("%s: initialization success\n", #name);                        \
    })
    ret = dtb_init(&dtb, (const void *)dtb_pa, dtb_pa);
    BOOTINFO(ret, DTB);

    ret = cpu_init(&dtb, hart);
    BOOTINFO(ret, CPU);

    ret = memblock_init(&dtb);
    BOOTINFO(ret, memblock);

    ret = page_init();
    BOOTINFO(ret, page);

    ret = pgtable_init(&dtb);
    BOOTINFO(ret, pgtable);

    ret = buddy_init();
    BOOTINFO(ret, buddy);

    slub_init();
    pr_info("slub: initialization success\n");

    ret = irq_init(&dtb);
    BOOTINFO(ret, IRQ);

    ret = tick_init(&dtb);
    BOOTINFO(ret, clock);

    timers_init();
    pr_info("timer: initialization success\n");

    ret = device_init(&dtb);
    BOOTINFO(ret, devices);

    ret = driver_init();
    BOOTINFO(ret, drivers);

    ret = smp_init();
    BOOTINFO(ret, SMP);

    ret = kthread_init();
    BOOTINFO(ret, kthread);

#undef BOOTINFO

    sched_task();

halt:
    panic("Kernel has been halted...\n");
}
