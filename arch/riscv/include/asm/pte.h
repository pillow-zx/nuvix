#ifndef _NUVIX_ASM_PTE_H
#define _NUVIX_ASM_PTE_H


#include <nuvix/compiler.h>
#include <nuvix/types.h>
#include <nuvix/bitops.h>
#include <asm/page.h>

typedef uint64_t pte_t;

#define PTE_V BIT(0)
#define PTE_R BIT(1)
#define PTE_W BIT(2)
#define PTE_X BIT(3)
#define PTE_U BIT(4)
#define PTE_G BIT(5)
#define PTE_A BIT(6)
#define PTE_D BIT(7)

#define PTE_TABLE PTE_V

#define PTE_KERN_R   (PTE_V | PTE_R | PTE_G | PTE_A | PTE_D)
#define PTE_KERN_RX  (PTE_V | PTE_X | PTE_G | PTE_A | PTE_D)
#define PTE_KERN_RW  (PTE_V | PTE_R | PTE_W | PTE_G | PTE_A | PTE_D)
#define PTE_KERN_RWX (PTE_V | PTE_R | PTE_W | PTE_X | PTE_G | PTE_A | PTE_D)

#define PTE_USER_R   (PTE_V | PTE_R | PTE_U | PTE_A | PTE_D)
#define PTE_USER_RX  (PTE_V | PTE_R | PTE_X | PTE_U | PTE_A | PTE_D)
#define PTE_USER_RW  (PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D)
#define PTE_USER_RWX (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D)

#define PTE_PPN_SHIFT	       10
#define PTE_PPN(pte)	       ((pte) >> PTE_PPN_SHIFT)
#define PTE_CREATE(pfn, flags) (((uint64_t)(pfn) << PTE_PPN_SHIFT) | (flags))
#define PTE_TO_PA(pte)	       ((paddr_t)PTE_PPN(pte) << PAGE_SHIFT)
#define PA_TO_PTE(pa)	       (PTE_CREATE(PHYS_PFN(pa), 0))

#endif
