#ifndef _NUVIX_UAPI_UCONTEXT_H
#define _NUVIX_UAPI_UCONTEXT_H

struct user_regs_struct {
	unsigned long pc;
	unsigned long ra;
	unsigned long sp;
	unsigned long gp;
	unsigned long tp;
	unsigned long t0;
	unsigned long t1;
	unsigned long t2;
	unsigned long s0;
	unsigned long s1;
	unsigned long a0;
	unsigned long a1;
	unsigned long a2;
	unsigned long a3;
	unsigned long a4;
	unsigned long a5;
	unsigned long a6;
	unsigned long a7;
	unsigned long s2;
	unsigned long s3;
	unsigned long s4;
	unsigned long s5;
	unsigned long s6;
	unsigned long s7;
	unsigned long s8;
	unsigned long s9;
	unsigned long s10;
	unsigned long s11;
	unsigned long t3;
	unsigned long t4;
	unsigned long t5;
	unsigned long t6;
};

struct riscv_f_ext_state {
	unsigned int f[32];
	unsigned int fcsr;
};

struct riscv_d_ext_state {
	unsigned long f[32];
	unsigned int fcsr;
};

struct riscv_q_ext_state {
	unsigned long f[64] __attribute__((aligned(16)));
	unsigned int fcsr;
	unsigned int reserved[3];
};

struct riscv_ctx_hdr {
	unsigned int magic;
	unsigned int size;
};

struct riscv_extra_ext_header {
	unsigned int padding[129] __attribute__((aligned(16)));
	unsigned int reserved;
	struct riscv_ctx_hdr hdr;
};

union riscv_fp_state {
	struct riscv_f_ext_state f;
	struct riscv_d_ext_state d;
	struct riscv_q_ext_state q;
	struct riscv_extra_ext_header ext;
};

struct sigcontext {
	struct user_regs_struct sc_regs;
	union riscv_fp_state sc_fpregs;
};

struct ucontext {
	unsigned long uc_flags;
	struct ucontext *uc_link;
	struct stack_t uc_stack;
	unsigned long uc_sigmask;
	unsigned char unused[120];
	struct sigcontext uc_mcontext;
};

_Static_assert(sizeof(struct user_regs_struct) == 256,
	       "Linux user_regs_struct size mismatch");
_Static_assert(__builtin_offsetof(struct ucontext, uc_mcontext) == 176,
	       "Linux ucontext mcontext offset mismatch");
_Static_assert(sizeof(struct ucontext) == 960,
	       "Linux riscv64 ucontext size mismatch");

#endif
