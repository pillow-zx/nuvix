#ifndef _NUVIX_ARCH_RISCV_SYSTEM_H
#define _NUVIX_ARCH_RISCV_SYSTEM_H

#include <nuvix/compiler.h>

enum system_reset_mode {
	ARCH_SYSTEM_RESET_RESTART,
	ARCH_SYSTEM_RESET_HALT,
	ARCH_SYSTEM_RESET_POWER_OFF,
};

__noreturn
void system_reset(enum system_reset_mode mode);

#endif
