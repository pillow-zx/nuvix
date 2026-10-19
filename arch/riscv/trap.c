#include <nuvix/assert.h>
#include <nuvix/cpu.h>
#include <nuvix/errno.h>
#include <nuvix/preempt.h>
#include <nuvix/sched.h>
#include <nuvix/tick.h>
#include <nuvix/irq.h>
#include <nuvix/mm.h>
#include <nuvix/printk.h>
#include <nuvix/syscall.h>
#include <nuvix/uaccess.h>
#include <asm/uaccess.h>
#include <asm/trap.h>

static_assert(sizeof(struct trap_frame) == TRAP_SIZE,
              "invalid trap frame size");
static_assert(offsetof(struct trap_frame, task) == TRAP_TASK,
              "invalid trap task offset");
IFDEF(CONFIG_FPU,
      static_assert(offsetof(struct trap_frame, fregs) == TRAP_FREG(0),
                    "invalid trap FPU offset");
      static_assert(offsetof(struct trap_frame, fcsr) == TRAP_FCSR,
                    "invalid trap FCSR offset");)

#define SUPERVISOR(frame) (frame->sstatus & SSTATUS_SPP)

#define IRQ_S_SOFT        SCAUSE_INTERRUPT | SCAUSE_SSI
#define IRQ_S_TIMER       SCAUSE_INTERRUPT | SCAUSE_STI
#define IRQ_S_EXT         SCAUSE_INTERRUPT | SCAUSE_SEI

#define die(frame, what, err)                                                  \
    do {                                                                       \
        if (SUPERVISOR(frame))                                                 \
            panic("%s in supervisor mode\nscause=%#lx sepc=%#lx stval=%#lx",   \
                  what, frame->scause, frame->sepc, frame->stval);             \
        local_irq_enable();                                                    \
        pr_err("task %d: %s (%d) scause=%#lx sepc=%#lx stval=%#lx\n",          \
               task_pid_nr(current, PIDTYPE_TID), what, err, frame->scause,    \
               frame->sepc, frame->stval);                                     \
        task_exit();                                                           \
    } while (0)

static bool handle_interrupt(struct trap_frame *frame)
{
    switch (frame->scause) {
    case IRQ_S_SOFT:
        return cpu_handle_ipi();
    case IRQ_S_TIMER:
        tick_handle();
        return true;
    case IRQ_S_EXT:
        arch_handle_external_irq();
        return true;
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

static bool uaccess_fault(const struct trap_frame *frame, unsigned long access)
{
    unsigned long pc;

    switch (access) {
    case VM_READ:
        pc = (unsigned long)__uaccess_load;
        break;
    case VM_WRITE:
        pc = (unsigned long)__uaccess_store;
        break;
    default:
        return false;
    }

    return frame->sepc == pc && SUPERVISOR(frame) &&
           access_ok((void *)frame->stval, 1) && current->mm;
}

static int do_page_fault(struct trap_frame *frame, unsigned long access)
{
    int ret;

    if (SUPERVISOR(frame) && !uaccess_fault(frame, access))
        return -EFAULT;

    if (!SUPERVISOR(frame) ||
        (SUPERVISOR(frame) && !preempt_count() && !this_cpu()->irq_depth)) {
        local_irq_enable();
        ret = handle_page_fault(current->mm, frame->stval, access);
        local_irq_disable();
        if (!ret || !SUPERVISOR(frame))
            return ret;
    }

    frame->sepc = (unsigned long)__uaccess_fixup;
    return 0;
}

static void do_syscall(struct trap_frame *frame)
{
    frame->sepc += 4;
    local_irq_enable();
    syscallarg(frame, 0) = syscall_dispatch(frame->regs[17], frame);
    local_irq_disable();
}

static void do_exception(struct trap_frame *frame)
{
    unsigned long access;
    int ret;

    switch (frame->scause) {
    case SCAUSE_ECALL_U:
        do_syscall(frame);
        return;
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
        die(frame, "unhandled exception", 0);
    }

    ret = do_page_fault(frame, access);
    if (ret)
        die(frame, "page fault", ret);
}

void trap_handler(struct trap_frame *frame)
{
    if (frame->scause & SCAUSE_INTERRUPT)
        do_interrupt(frame);
    else
        do_exception(frame);
}
