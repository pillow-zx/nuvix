#ifndef _NUVIX_REBOOT_H
#define _NUVIX_REBOOT_H

#include <nuvix/compiler.h>

/* Architecture reset mechanism, after the reboot policy has accepted it. */
enum reset_mode {
	RESET_RESTART,
	RESET_HALT,
	RESET_POWEROFF,
};

__noreturn
void system_reset(enum reset_mode mode);

enum reboot_command {
	REBOOT_CAD_OFF,
	REBOOT_CAD_ON,
	REBOOT_RESTART,
	REBOOT_HALT,
	REBOOT_POWER_OFF,
};

__must_check
int reboot(enum reboot_command command);

#endif
