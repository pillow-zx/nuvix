#ifndef _NUVIX_RISCV_SIGNAL_H
#define _NUVIX_RISCV_SIGNAL_H

#include <nuvix/signal.h>

#define SIGNAL_STACK_ALIGN 16
#define END_MAGIC          0
#define END_HDR_SIZE       0

struct riscv_fp_state {
    uint64_t f[32];
    uint32_t fcsr;
    uint32_t padding[64];
    uint32_t reserved;
    uint32_t magic;
    uint32_t size;
} __aligned(SIGNAL_STACK_ALIGN);

struct sigcontext {
    unsigned long sc_regs[32];
    struct riscv_fp_state sc_fpregs;
};

typedef struct sigaltstack {
    void *ss_sp;
    int ss_flags;
    size_t ss_size;
} stack_t;

struct ucontext {
    unsigned long uc_flags;
    struct ucontext *uc_link;
    stack_t uc_stack;
    sigset_t uc_sigmask;
    uint8_t unused[128 - sizeof(sigset_t)];
    struct sigcontext uc_mcontext;
};

struct rt_sigframe {
    siginfo_t info;
    struct ucontext uc;
};

struct trap_frame;

void arch_do_signal(struct trap_frame *frame, const siginfo_t *fault);

#endif
