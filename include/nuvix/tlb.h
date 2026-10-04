#ifndef _NUVIX_TLB_H
#define _NUVIX_TLB_H

#include <asm/tlb.h>

#define local_flush_tlb_all()      arch_local_flush_tlb_all()
#define local_flush_tlb_page(addr) arch_local_flush_tlb_page(addr)

#endif /* _NUVIX_TLB_H */
