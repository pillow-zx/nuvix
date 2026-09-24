#ifndef _NUVIX_RISCV_ENTRY_H
#define _NUVIX_RISCV_ENTRY_H

#include <nuvix/compiler.h>
#include <nuvix/types.h>

struct trap_frame;
struct fpu_state;

void __alltraps(void);

void trap_handler(struct trap_frame *tf);

struct trap_frame *trap_prepare_return(struct trap_frame *tf);

void fpu_save(struct fpu_state *state);

void fpu_restore(const struct fpu_state *state);

__must_check
bool uaccess_fixup(struct trap_frame *tf);

#endif
