#ifndef _NUVIX_LAYOUT_H
#define _NUVIX_LAYOUT_H

#include <asm/layout.h>

#define UVADDR_START 0x10000

#define __va(addr) (KERNEL_VBASE + (addr - KERNEL_PBASE))

#define __pa(addr) (KERNEL_PBASE + (addr - KERNEL_VBASE))

#endif /* ifndef _NUVIX_LAYOUT_H */
