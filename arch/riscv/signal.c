#include <nuvix/signal.h>
#include <nuvix/task.h>
#include <nuvix/string.h>
#include <nuvix/uaccess.h>
#include <nuvix/syscall.h>
#include <nuvix/math.h>
#include <nuvix/layout.h>
#include <asm/signal.h>
#include <asm/trap.h>

static void setup_signal_frame(struct trap_frame *frame,
                               const struct ksignal *signal)
{
    struct rt_sigframe sf = {0};
    struct sigcontext *sc = &sf.uc.uc_mcontext;
    struct rt_sigframe *usf;
    const stack_t *stack = &current->sigstack;
    unsigned long sp = frame->regs[2];
    int stack_flags = signal_stack_flags(sp);
    bool on_sigstack = stack_flags == SS_ONSTACK;

    sf.info = signal->info;
    sf.uc.uc_sigmask = signal->oldmask;
    sf.uc.uc_stack.ss_sp = stack->ss_sp;
    sf.uc.uc_stack.ss_size = stack->ss_size;
    sf.uc.uc_stack.ss_flags = stack_flags;
    for (unsigned int i = 0; i < 32; ++i)
        sc->sc_regs[i] = frame->regs[i];
    sc->sc_regs[0] = frame->sepc;
#ifdef CONFIG_FPU
    if (frame->sstatus & SSTATUS_FS) {
        memcpy(sc->sc_fpregs.f, frame->fregs, sizeof(sc->sc_fpregs.f));
        sc->sc_fpregs.fcsr = frame->fcsr;
    }
#endif
    sc->sc_fpregs.magic = END_MAGIC;
    sc->sc_fpregs.size = END_HDR_SIZE;

    if ((signal->action.sa_flags & SA_ONSTACK) && !stack_flags) {
        if (check_add_overflow((unsigned long)stack->ss_sp, stack->ss_size,
                               &sp))
            task_exit(SIGSEGV);
        on_sigstack = true;
    }
    if (sp < sizeof(sf))
        task_exit(SIGSEGV);
    sp = ALIGN_DOWN(sp - sizeof(sf), SIGNAL_STACK_ALIGN);
    if (on_sigstack && sp < (unsigned long)stack->ss_sp)
        task_exit(SIGSEGV);
    if (copy_to_user((void *)sp, &sf, sizeof(sf)))
        task_exit(SIGSEGV);

    signal_delivered(signal);
    usf = (void *)sp;
    frame->sepc = (unsigned long)signal->action.sa_handler;
    frame->regs[1] = USER_TRAMPOLINE;
    frame->regs[2] = sp;
    frame->regs[10] = sf.info.si_signo;
    frame->regs[11] = (unsigned long)&usf->info;
    frame->regs[12] = (unsigned long)&usf->uc;
}

void arch_do_signal(struct trap_frame *frame, const siginfo_t *fault)
{
    struct ksignal signal;

    if (!fault && !signal_pending(current) &&
        !task_test_flag(current, TF_SIGSUSPEND | TF_RESIGMASK))
        return;
    if (get_signal(&signal, fault))
        setup_signal_frame(frame, &signal);
}

long sys_rt_sigreturn(struct trap_frame *frame)
{
    struct task *task = current;
    struct rt_sigframe sf;
    const struct sigcontext *sc;

    if (copy_from_user(&sf, (void *)frame->regs[2], sizeof(sf)))
        task_exit(SIGSEGV);

    sc = &sf.uc.uc_mcontext;
    frame->sepc = sc->sc_regs[0];
    for (unsigned i = 1; i < 32; ++i)
        frame->regs[i] = sc->sc_regs[i];
    /* Linux leaves the configuration unchanged on restore validation errors. */
    (void)signal_set_stack(&sf.uc.uc_stack, frame->regs[2]);
#ifdef CONFIG_FPU
    memcpy(frame->fregs, sc->sc_fpregs.f, sizeof(frame->fregs));
    frame->fcsr = sc->sc_fpregs.fcsr;
    frame->sstatus = (frame->sstatus & ~SSTATUS_FS) | SSTATUS_FS_CLEAN;
#endif
    frame->scause = -1UL;

    spin_lock(&task->signal_lock);
    task->blocked = sf.uc.uc_sigmask & ~SIG_UNBLOCKMASK;
    recalc_sigpending(task);
    spin_unlock(&task->signal_lock);

    return frame->regs[10];
}
