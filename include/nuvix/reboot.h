#ifndef _NUVIX_REBOOT_H
#define _NUVIX_REBOOT_H

#include <nuvix/compiler.h>

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
