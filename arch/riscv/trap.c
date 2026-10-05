#include <nuvix/assert.h>
#include <asm/trap.h>
#include <nuvix/cpu.h>
#include <nuvix/preempt.h>
#include <nuvix/sched.h>
#include <nuvix/tick.h>

void trap_handler(const struct trap_frame *frame)
{
    switch (frame->scause) {
    case SCAUSE_INTERRUPT | SCAUSE_SSI:
    case SCAUSE_INTERRUPT | SCAUSE_STI: {
        struct cpu *cpu = &cpus[current_cpu_id()];
        bool check_resched;

        preempt_disable();
        ++cpu->irq_depth;
        if (frame->scause == (SCAUSE_INTERRUPT | SCAUSE_SSI)) {
            check_resched = cpu_handle_ipi();
        } else {
            tick_handle();
            check_resched = true;
        }
        --cpu->irq_depth;
        preempt_enable_no_resched();
        if (check_resched)
            sched_irq_exit();
        break;
    }
    default:
        panic("unhandled supervisor %s\nscause=%p sepc=%p stval=%p",
              frame->scause & SCAUSE_INTERRUPT ? "interrupt" : "exception",
              (void *)frame->scause, (void *)frame->sepc, (void *)frame->stval);
    }
}
