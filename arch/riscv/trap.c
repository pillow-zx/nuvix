#include <nuvix/assert.h>
#include <asm/trap.h>
#include <nuvix/cpu.h>
#include <nuvix/preempt.h>
#include <nuvix/sched.h>
#include <nuvix/tick.h>
#include <nuvix/irq.h>

#define IRQ_S_SOFT  SCAUSE_INTERRUPT | SCAUSE_SSI
#define IRQ_S_TIMER SCAUSE_INTERRUPT | SCAUSE_STI
#define IRQ_S_EXT   SCAUSE_INTERRUPT | SCAUSE_SEI

static bool handle_interrupt(struct trap_frame *frame)
{
    switch (frame->scause) {
    case IRQ_S_SOFT:
        return cpu_handle_ipi();
    case IRQ_S_TIMER: {
        tick_handle();
        return true;
    }
    case IRQ_S_EXT: {
        arch_handle_external_irq();
        return true;
    }
    default:
        panic("unhandled supervisor interrupt\nscause=%p sepc=%p stval=%p",
              (void *)frame->scause, (void *)frame->sepc, (void *)frame->stval);
    }
}

static void do_interrupt(struct trap_frame *frame)
{
    struct cpu *cpu = this_cpu();
    bool check_resched;

    preempt_disable();
    ++cpu->irq_depth;
    check_resched = handle_interrupt(frame);
    --cpu->irq_depth;
    preempt_enable_no_resched();
    if (check_resched)
        sched_irq_exit();
}

static void do_exception(struct trap_frame *frame)
{
    panic("unhandled supervisor exception\nscause=%p sepc=%p stval=%p",
          (void *)frame->scause, (void *)frame->sepc, (void *)frame->stval);
}

void trap_handler(struct trap_frame *frame)
{
    if (frame->scause & SCAUSE_INTERRUPT)
        do_interrupt(frame);
    else
        do_exception(frame);
}
