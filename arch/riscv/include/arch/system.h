#ifndef _NUVIX_ARCH_RISCV_SYSTEM_H
#define _NUVIX_ARCH_RISCV_SYSTEM_H

#include <nuvix/compiler.h>

enum reset_mode {
	RESET_RESTART,
	RESET_HALT,
	RESET_POWEROFF,
};

__noreturn
void system_reset(enum reset_mode mode);

#endif
