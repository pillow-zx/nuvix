#ifndef _NUVIX_RISCV_MMU_H
#define _NUVIX_RISCV_MMU_H

#include <nuvix/types.h>

extern uintptr_t kernel_satp;
void active_pgtable(uintptr_t satp);

#endif
