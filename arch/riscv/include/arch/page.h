#ifndef _NUVIX_ARCH_RISCV_PAGE_H
#define _NUVIX_ARCH_RISCV_PAGE_H


#include <arch/config.h>
#include <asm/page.h>
#include <nuvix/compiler.h>
#include <nuvix/types.h>

#define __pa(x) ((vaddr_t)(uintptr_t)(x) - KERNEL_VBASE)

#define __va(x) ((void *)((paddr_t)(x) + KERNEL_VBASE))

void pgtable_init(void);

#endif
