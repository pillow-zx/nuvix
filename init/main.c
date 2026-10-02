#include <nuvix/ctypes.h>
#include <nuvix/compiler.h>

__noreturn
void kernel_main(unsigned long hart, uintptr_t dtb)
{
    (void)hart;
    (void)dtb;

    for (;;)
        __asm__ volatile("wfi" ::: "memory");
}
