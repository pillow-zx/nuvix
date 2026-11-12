#include <nuvix/assert.h>
#include <nuvix/ctypes.h>
#include <nuvix/printk.h>
#include <nuvix/irq.h>
#include <nuvix/cpu.h>
#include <nuvix/trace.h>

__noreturn void __panic(const char *fmt, ...)
{
    static atomic_t panicking = ATOMIC_INIT(0);
    va_list args;
    int ret;
    bool already_panicking;

    local_irq_disable();
    already_panicking = atomic_xchg(&panicking, 1);
    IFDEF(CONFIG_SMP, cpu_boot_fail());
    if (already_panicking)
        halt();

    printk("\nPANIC: ");
    va_start(args, fmt);
    ret = vprintk(fmt, args);
    va_end(args);
    if (ret < 0)
        printk("<message formatting failed>");
    printk("\n");
    IFDEF(CONFIG_TRACE_CPURING, trace_dump());
    halt();
}
