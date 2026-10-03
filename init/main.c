#include <nuvix/ctypes.h>
#include <nuvix/compiler.h>
#include <nuvix/macro.h>
#include <nuvix/printk.h>
#include <nuvix/dtb.h>
#include <nuvix/memblock.h>

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
    pr_info("Hello, Nuvix!\n");

halt:
    for (;;)
        __asm__ volatile("wfi" ::: "memory");
}
