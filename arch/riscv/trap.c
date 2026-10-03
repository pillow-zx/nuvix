#include <nuvix/assert.h>
#include <nuvix/printk.h>
#include <nuvix/arch/riscv/trap.h>
#include <nuvix/arch/riscv/cpu.h>

__noreturn
void trap_handler(const struct trap_frame *frame)
{
    arch_halt();
}
