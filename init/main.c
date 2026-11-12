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
#include <nuvix/task.h>
#include <nuvix/irq.h>
#include <nuvix/device.h>
#include <nuvix/mapping.h>
#include <nuvix/futex.h>

paddr_t start_kernel(unsigned long hart, paddr_t dtb_pa);
void kernel_main(void) __noreturn;

#define BOOTINFO(ret, name)                                                    \
    ({                                                                         \
        if (ret) {                                                             \
            pr_err("%s: initialization failed (%d)\n", #name, ret);            \
            goto halt;                                                         \
        }                                                                      \
        pr_info("%s: initialization success\n", #name);                        \
    })

paddr_t start_kernel(unsigned long hart, paddr_t dtb_pa)
{
    paddr_t root;
    int ret;

    ret = dtb_init((const void *)dtb_pa, dtb_pa);
    BOOTINFO(ret, DTB);

    ret = cpu_init(hart);
    BOOTINFO(ret, CPU);

    ret = memblock_init();
    BOOTINFO(ret, memblock);

    ret = page_init();
    BOOTINFO(ret, page);

    ret = pgtable_init(&root);
    BOOTINFO(ret, pgtable);

    /* All MMU-off accesses are complete; prepare pointers for the MMU switch. */
    mem_map = (struct page *)__va((uintptr_t)mem_map);
    dtb.blob = (const void *)__va(dtb.paddr);
    return root;

halt:
    panic("Kernel has been halted...\n");
}

void kernel_main(void)
{
    int ret;

    ret = buddy_init();
    BOOTINFO(ret, buddy);

    slub_init();
    pr_info("slub: initialization success\n");

    ret = irq_init();
    BOOTINFO(ret, IRQ);

    ret = tick_init();
    BOOTINFO(ret, clock);

    timers_init();
    pr_info("timer: initialization success\n");

    IFDEF(CONFIG_FUTEX, futex_init());

    ret = device_init();
    BOOTINFO(ret, devices);

    ret = driver_init();
    BOOTINFO(ret, drivers);

    ret = cpu_startup();
    BOOTINFO(ret, CPU startup);

    ret = mapping_init();
    BOOTINFO(ret, mapping);

    ret = ktask_init();
    BOOTINFO(ret, kthread);

    sched_task();

halt:
    panic("Kernel has been halted...\n");
}

#undef BOOTINFO
