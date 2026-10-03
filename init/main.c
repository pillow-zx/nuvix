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
void kernel_main(unsigned long hart, uintptr_t dtb_pa)
{
    struct dtb dtb;
    int ret;

    (void)hart;
    IFDEF(CONFIG_UART, uart_init());
    ret = dtb_init(&dtb, (const void *)dtb_pa, dtb_pa);
    if (ret) {
        pr_err("DTB: initialization failed (%d)\n", ret);
        goto halt;
    }

    ret = memblock_init(&dtb);
    if (ret) {
        pr_err("memblock: initialization failed (%d)\n", ret);
        goto halt;
    }

    ret = page_init(BOOT_IDMAP_PBASE, BOOT_IDMAP_PEND);
    if (ret) {
        pr_err("page: initialization failed (%d)\n", ret);
        goto halt;
    }
    pr_info("Hello, Nuvix!\n");

halt:
    panic("Kernel has been halted...\n");
}
