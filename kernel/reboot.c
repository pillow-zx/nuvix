/*
 * kernel/reboot.c - privileged system reboot policy
 */

#include <nuvix/errno.h>
#include <nuvix/reboot.h>
#include <nuvix/task.h>

int reboot(enum reboot_command command)
{
	if (!current_task() || task_uid(current_task()) != 0)
		return -EPERM;

	switch (command) {
	case REBOOT_CAD_OFF:
	case REBOOT_CAD_ON:
		return 0;
	case REBOOT_RESTART:
		system_reset(RESET_RESTART);
	case REBOOT_HALT:
		system_reset(RESET_HALT);
	case REBOOT_POWER_OFF:
		system_reset(RESET_POWEROFF);
	}

	return -EINVAL;
}
