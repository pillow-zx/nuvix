#include <nuvix/assert.h>
#include <nuvix/ctypes.h>
#include <nuvix/printk.h>
#include <nuvix/irq.h>
#include <nuvix/cpu.h>

__noreturn
void __panic(const char *fmt, ...)
{
    /* The current kernel runs only the boot hart. */
    static bool panicking;
    va_list args;
    int ret;

    local_irq_disable();
    if (panicking)
        halt();
    panicking = true;

    printk("\nPANIC: ");
    va_start(args, fmt);
    ret = vprintk(fmt, args);
    va_end(args);
    if (ret < 0)
        printk("<message formatting failed>");
    printk("\n");
    halt();
}
