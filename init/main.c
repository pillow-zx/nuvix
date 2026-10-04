#include <nuvix/ctypes.h>
#include <nuvix/compiler.h>
#include <nuvix/macro.h>
#include <nuvix/printk.h>
#include <nuvix/dtb.h>
#include <nuvix/memblock.h>
#include <nuvix/page.h>
#include <nuvix/assert.h>
#include <nuvix/pgtable.h>

__noreturn
void kernel_main(unsigned long hart, paddr_t dtb_pa)
{
    struct dtb dtb;
    int ret;

    (void)hart;
    IFDEF(CONFIG_UART, uart_init());

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

    ret = memblock_init(&dtb);
    BOOTINFO(ret, memblock);

    ret = page_init();
    BOOTINFO(ret, page);

    ret = pgtable_init(&dtb);
    BOOTINFO(ret, pgtable);

#undef BOOTINFO

halt:
    panic("Kernel has been halted...\n");
}
