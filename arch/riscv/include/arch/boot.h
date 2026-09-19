#ifndef _NUVIX_ARCH_BOOT_H
#define _NUVIX_ARCH_BOOT_H

#include <nuvix/types.h>

void dtb_init(paddr_t address);
void dt_cpus_init(uint64_t boot_hartid);
void platform_init(uint64_t boot_hartid, paddr_t dtb_pa);

#endif
