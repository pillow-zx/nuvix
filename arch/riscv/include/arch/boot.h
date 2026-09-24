#ifndef _NUVIX_ARCH_BOOT_H
#define _NUVIX_ARCH_BOOT_H

#include <nuvix/types.h>

void platform_init(uint64_t boot_hartid, paddr_t dtb_pa);
void platform_irq_init(void);

#endif
