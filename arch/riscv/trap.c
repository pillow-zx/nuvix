#include <nuvix/cpu.h>
#include <nuvix/errno.h>
#include <nuvix/preempt.h>
#include <nuvix/sched.h>
#include <nuvix/tick.h>
#include <nuvix/irq.h>
#include <nuvix/mm.h>
#include <nuvix/syscall.h>
#include <nuvix/uaccess.h>
#include <nuvix/assert.h>
#include <nuvix/task.h>
#include <nuvix/signal.h>
#include <asm/signal.h>
#include <asm/uaccess.h>
#include <asm/trap.h>

#define SUPERVISOR(frame) (frame->sstatus & SSTATUS_SPP)

#define IRQ_S_SOFT        SCAUSE_INTERRUPT | SCAUSE_SSI
#define IRQ_S_TIMER       SCAUSE_INTERRUPT | SCAUSE_STI
#define IRQ_S_EXT         SCAUSE_INTERRUPT | SCAUSE_SEI

static bool handle_interrupt(struct trap_frame *frame)
{
    switch (frame->scause) {
#ifdef CONFIG_SMP
    case IRQ_S_SOFT:
        return cpu_handle_ipi();
#endif
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

static void set_fault_signal(struct trap_frame *frame, siginfo_t *fault,
                             int sig, int code, unsigned long addr)
{
    if (SUPERVISOR(frame))
        panic(
            "unhandled supervisor exception\nscause=%#lx sepc=%#lx stval=%#lx",
            frame->scause, frame->sepc, frame->stval);
    fault->si_signo = sig;
    fault->si_code = code;
    fault->si_addr = (void *)addr;
}

static void do_exception(struct trap_frame *frame, siginfo_t *fault)
{
    unsigned long access;
    int ret;

    switch (frame->scause) {
    case SCAUSE_ECALL_U:
        do_syscall(frame);
        return;
    case SCAUSE_ILLEGAL_INST:
        set_fault_signal(frame, fault, SIGILL, ILL_ILLOPC, frame->sepc);
        return;
    case SCAUSE_BREAKPOINT:
        set_fault_signal(frame, fault, SIGTRAP, TRAP_BRKPT, frame->sepc);
        return;
    case SCAUSE_INST_MISALIGNED:
    case SCAUSE_LOAD_MISALIGNED:
    case SCAUSE_STORE_MISALIGNED:
        set_fault_signal(frame, fault, SIGBUS, BUS_ADRALN, frame->sepc);
        return;
    case SCAUSE_INST_ACCESS_FAULT:
    case SCAUSE_LOAD_ACCESS_FAULT:
    case SCAUSE_STORE_ACCESS_FAULT:
        set_fault_signal(frame, fault, SIGSEGV, SEGV_ACCERR, frame->sepc);
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
        set_fault_signal(frame, fault, SIGILL, ILL_ILLTRP, frame->sepc);
        return;
    }

    ret = do_page_fault(frame, access);
    if (!ret)
        return;
    if (ret == -ENOMEM) {
        local_irq_enable();
        task_exit(SIGSEGV);
    }
    if (ret == -EFAULT)
        set_fault_signal(frame, fault, SIGSEGV, SEGV_MAPERR, frame->stval);
    else if (ret == -EACCES)
        set_fault_signal(frame, fault, SIGSEGV, SEGV_ACCERR, frame->stval);
    else
        set_fault_signal(frame, fault, SIGBUS, BUS_ADRERR, frame->stval);
}

void trap_handler(struct trap_frame *frame)
{
    siginfo_t fault = {0};

    if (frame->scause & SCAUSE_INTERRUPT)
        do_interrupt(frame);
    else
        do_exception(frame, &fault);

    if (!SUPERVISOR(frame)) {
        local_irq_enable();
        arch_do_signal(frame, fault.si_signo ? &fault : NULL);
        local_irq_disable();
    }
}
