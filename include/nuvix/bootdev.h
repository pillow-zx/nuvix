#ifndef _NUVIX_BOOTDEV_H
#define _NUVIX_BOOTDEV_H

#include <nuvix/types.h>

/* Discover/map devices before SMP or any process page table is created. */
void boot_devices_prepare(void);
/* Start the console IRQ after the boot CPU and PLIC are online. */
int boot_console_start(void);
/* Register the selected disk after block/VFS initialization. */
dev_t boot_disk_init(void);

#endif
