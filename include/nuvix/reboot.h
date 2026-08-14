#ifndef _NUVIX_REBOOT_H
#define _NUVIX_REBOOT_H

#include <nuvix/compiler.h>

enum kernel_reboot_command {
	KERNEL_REBOOT_CAD_OFF,
	KERNEL_REBOOT_CAD_ON,
	KERNEL_REBOOT_RESTART,
	KERNEL_REBOOT_HALT,
	KERNEL_REBOOT_POWER_OFF,
};

__must_check
int kernel_reboot(enum kernel_reboot_command command);

#endif
