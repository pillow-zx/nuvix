#include <nuvix/assert.h>
#include <asm/trap.h>

__noreturn
void trap_handler(const struct trap_frame *frame)
{
    panic("unhandled supervisor %s\nscause=%p sepc=%p stval=%p",
          frame->scause & SCAUSE_INTERRUPT ? "interrupt" : "exception",
          (void *)frame->scause, (void *)frame->sepc, (void *)frame->stval);
}
