/*
 * kernel/reboot.c - privileged system reboot policy
 */

#include <arch/system.h>
#include <nuvix/errno.h>
#include <nuvix/reboot.h>
#include <nuvix/task.h>

int kernel_reboot(enum kernel_reboot_command command)
{
	if (!current_task() || task_uid(current_task()) != 0)
		return -EPERM;

	switch (command) {
	case KERNEL_REBOOT_CAD_OFF:
	case KERNEL_REBOOT_CAD_ON:
		return 0;
	case KERNEL_REBOOT_RESTART:
		system_reset(ARCH_SYSTEM_RESET_RESTART);
	case KERNEL_REBOOT_HALT:
		system_reset(ARCH_SYSTEM_RESET_HALT);
	case KERNEL_REBOOT_POWER_OFF:
		system_reset(ARCH_SYSTEM_RESET_POWER_OFF);
	}

	return -EINVAL;
}
