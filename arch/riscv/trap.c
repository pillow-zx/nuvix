#include <nuvix/assert.h>
#include <asm/trap.h>
#include <nuvix/cpu.h>
#include <nuvix/preempt.h>
#include <nuvix/sched.h>
#include <nuvix/tick.h>
#include <nuvix/irq.h>
#include <nuvix/mm.h>
#include <nuvix/printk.h>

static_assert(sizeof(struct trap_frame) == TRAP_SIZE,
              "invalid trap frame size");
static_assert(offsetof(struct trap_frame, task) == TRAP_TASK,
              "invalid trap task offset");
IFDEF(CONFIG_FPU,
      static_assert(offsetof(struct trap_frame, fregs) == TRAP_FREG(0),
                    "invalid trap FPU offset");
      static_assert(offsetof(struct trap_frame, fcsr) == TRAP_FCSR,
                    "invalid trap FCSR offset");)

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
    unsigned long access;
    int ret;

    if (frame->sstatus & SSTATUS_SPP)
        panic("unhandled supervisor exception\nscause=%p sepc=%p stval=%p",
              (void *)frame->scause, (void *)frame->sepc, (void *)frame->stval);

    switch (frame->scause) {
    case SCAUSE_INST_PAGE_FAULT:
        access = VM_EXEC;
        break;
    case SCAUSE_LOAD_PAGE_FAULT:
        access = VM_READ;
        break;
    case SCAUSE_STORE_PAGE_FAULT:
        access = VM_WRITE;
        break;
    default:
        local_irq_enable();
        pr_err("task %u: unhandled user exception scause=%p sepc=%p stval=%p\n",
               current->tid, (void *)frame->scause, (void *)frame->sepc,
               (void *)frame->stval);
        preempt_disable();
        sched_exit();
    }

    local_irq_enable();
    ret = handle_page_fault(current->mm, frame->stval, access);
    if (ret) {
        pr_err("task %u: user page fault (%d) scause=%p sepc=%p stval=%p\n",
               current->tid, ret, (void *)frame->scause, (void *)frame->sepc,
               (void *)frame->stval);
        preempt_disable();
        sched_exit();
    }
    local_irq_disable();
}

void trap_handler(struct trap_frame *frame)
{
    if (frame->scause & SCAUSE_INTERRUPT)
        do_interrupt(frame);
    else
        do_exception(frame);
}
