#include <nuvix/ctypes.h>
#include <nuvix/printk.h>
#include <nuvix/dtb.h>
#include <nuvix/cpu.h>
#include <nuvix/memblock.h>
#include <nuvix/page.h>
#include <nuvix/assert.h>
#include <nuvix/pgtable.h>

paddr_t start_kernel(unsigned long hart, paddr_t dtb_pa);

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

#undef BOOTINFO
