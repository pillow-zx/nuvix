#ifndef _NUVIX_ARCH_RISCV_PGTABLE_H
#define _NUVIX_ARCH_RISCV_PGTABLE_H

#include <nuvix/compiler.h>
#include <nuvix/types.h>
#include <arch/page.h>
#include <asm/csr.h>
#include <asm/pte.h>

typedef pte_t pgprot_t;

#define pgprot_user(read, write, exec)                                  \
	((pgprot_t)(PTE_V | PTE_U | PTE_A | PTE_D | ((read) ? PTE_R : 0) |     \
		    ((write) ? (PTE_R | PTE_W) : 0) | ((exec) ? PTE_X : 0)))

#define pgprot_kernel(read, write, exec)                                \
	((pgprot_t)(PTE_V | PTE_G | PTE_A | PTE_D | ((read) ? PTE_R : 0) |     \
		    ((write) ? (PTE_R | PTE_W) : 0) | ((exec) ? PTE_X : 0)))

#define pgprot_ro(prot) ((pgprot_t)((prot) & ~PTE_W))

#define pte_present(pte) (((pte) & PTE_V) != 0)

#define pte_upage(pte)                                                         \
	({                                                                     \
		pte_t __pte_value = (pte);                                     \
		(__pte_value & PTE_U) != 0 && PTE_TO_PA(__pte_value) != 0;     \
	})

#define pte_uread(pte)                                                         \
	(((pte) & (PTE_V | PTE_U | PTE_R)) == (PTE_V | PTE_U | PTE_R))

#define pte_uwrite(pte)                                                        \
	(((pte) & (PTE_V | PTE_U | PTE_W)) == (PTE_V | PTE_U | PTE_W))

#define pte_uexec(pte)                                                         \
	(((pte) & (PTE_V | PTE_U | PTE_X)) == (PTE_V | PTE_U | PTE_X))

#define pte_prot(pte) ((pgprot_t)((pte) & MASK(PTE_PPN_SHIFT)))

#define pte_phys(pte) PTE_TO_PA(pte)

#define pte_make(pa, prot) ((pte_t)(PA_TO_PTE(pa) | (prot)))

#define pte_clear(pte)                                                         \
	do {                                                                   \
		*(pte) &= ~PTE_V;                                              \
	} while (0)

#define pgtable_token(pgd)                                             \
	((uintptr_t)(SATP_MODE_SV39 | (__pa((uintptr_t)(pgd)) >> PAGE_SHIFT)))

#endif
