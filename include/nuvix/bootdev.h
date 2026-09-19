#ifndef _NUVIX_BOOTDEV_H
#define _NUVIX_BOOTDEV_H

#include <nuvix/types.h>

/* Discover/map devices before SMP or any process page table is created. */
void boot_devices_prepare(void);
/* Register the selected disk after block/VFS initialization. */
dev_t boot_disk_init(void);

#endif
