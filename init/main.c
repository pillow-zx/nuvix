#include <nuvix/ctypes.h>
#include <nuvix/compiler.h>
#include <nuvix/macro.h>
#include <nuvix/printk.h>

__noreturn
void kernel_main(unsigned long hart, uintptr_t dtb)
{
    (void)hart;
    (void)dtb;

    IFDEF(CONFIG_UART, uart_init());
    printk("Hello, Nuvix!\n");

    for (;;)
        __asm__ volatile("wfi" ::: "memory");
}
