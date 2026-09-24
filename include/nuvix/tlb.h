#ifndef _NUVIX_TLB_H
#define _NUVIX_TLB_H

#include <nuvix/types.h>

/* Local CPU only. The MM layer coordinates remote shootdowns. */
void tlb_flush_all(void);
void tlb_flush_page(uintptr_t va);

#endif
