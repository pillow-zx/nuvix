#ifndef _NUVIX_ASM_TLB_H
#define _NUVIX_ASM_TLB_H

#include <nuvix/types.h>

void tlb_flush_all(void);
void tlb_flush_page(uintptr_t va);
void icache_flush(void);

#endif
