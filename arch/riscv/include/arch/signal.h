#ifndef _NUVIX_ARCH_RISCV_SIGNAL_H
#define _NUVIX_ARCH_RISCV_SIGNAL_H

#include <nuvix/types.h>
#include <uapi/siginfo.h>

struct trap_frame;
struct sigaction;
struct stack_t;

/* Encode/decode the user ABI; the signal core owns blocked-mask policy. */
int arch_signal_setup(struct trap_frame *tf, int sig, const siginfo_t *info,
		      const struct sigaction *action, uint64_t mask);
int arch_signal_restore(struct trap_frame *tf, uintptr_t sp,
			uint64_t *mask, struct stack_t *stack);

#endif
