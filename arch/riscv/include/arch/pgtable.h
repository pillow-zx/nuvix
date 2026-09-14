#ifndef _NUVIX_ARCH_RISCV_PGTABLE_H
#define _NUVIX_ARCH_RISCV_PGTABLE_H

#include <nuvix/compiler.h>
#include <nuvix/types.h>
#include <nuvix/atomic.h>
#include <arch/page.h>
#include <asm/csr.h>
#include <asm/pte.h>
#include <asm/tlb.h>

typedef pte_t pgprot_t;

extern uintptr_t kpgroot;

pte_t *pgtable_ucreate(void);
void pgtable_udestroy(pte_t *root);
/* Caller exclusively owns user mappings. Clears the next user leaf, including
 * software non-present leaves. Transfers its reference to the caller, which
 * must finish shootdown before releasing it. Cursor starts at zero. */
bool pgtable_take_upage(pte_t *root, uintptr_t *cursor, paddr_t *pa);
/* Boot-only, before concurrent use; leaves prepared entries unmapped. */
int pgtable_prepare_range(pte_t *root, uintptr_t start, uintptr_t end);

void active_pgtable(uintptr_t root);

__must_check pte_t *kpgtable(void);

extern atomic_isize_t pt_boot_token;

__must_check uintptr_t pt_boot_token_acquire(void);

__must_check bool pt_boot_token_valid(void);

__must_check __nonnull(1) pte_t *pt_lookup(pte_t *root, uintptr_t va);

__must_check __nonnull(1) int map_page(pte_t *root, uintptr_t va, uintptr_t pa,
				       uint64_t perm);

__must_check int arch_upgd_region(vaddr_t *start, vaddr_t *end);

__must_check int arch_upgd_init(pte_t *root);

#define pgprot_user(read, write, exec)                                         \
	((pgprot_t)(PTE_V | PTE_U | PTE_A | PTE_D | ((read) ? PTE_R : 0) |     \
		    ((write) ? (PTE_R | PTE_W) : 0) | ((exec) ? PTE_X : 0)))

#define pgprot_kernel(read, write, exec)                                       \
	((pgprot_t)(PTE_V | PTE_G | PTE_A | PTE_D | ((read) ? PTE_R : 0) |     \
		    ((write) ? (PTE_R | PTE_W) : 0) | ((exec) ? PTE_X : 0)))

#define pgprot_ro(prot) ((pgprot_t)((prot) & ~PTE_W))

#define pte_present(pte) (((pte) & PTE_V) != 0)

#define pte_user_page(pte)                                                     \
	({                                                                     \
		pte_t __pte_value = (pte);                                     \
		(__pte_value & PTE_U) != 0 && PTE_TO_PA(__pte_value) != 0;     \
	})

#define pte_user_read(pte)                                                     \
	(((pte) & (PTE_V | PTE_U | PTE_R)) == (PTE_V | PTE_U | PTE_R))

#define pte_user_write(pte)                                                    \
	(((pte) & (PTE_V | PTE_U | PTE_W)) == (PTE_V | PTE_U | PTE_W))

#define pte_user_exec(pte)                                                     \
	(((pte) & (PTE_V | PTE_U | PTE_X)) == (PTE_V | PTE_U | PTE_X))

#define pte_prot(pte) ((pgprot_t)((pte) & MASK(PTE_PPN_SHIFT)))

#define pte_make(pa, prot) ((pte_t)(PA_TO_PTE(pa) | (prot)))

#define pte_clear(pte)                                                         \
	do {                                                                   \
		*(pte) &= ~PTE_V;                                              \
	} while (0)

#define pt_token(pgd)                                                          \
	((uintptr_t)(SATP_MODE_SV39 | (__pa((uintptr_t)(pgd)) >> PAGE_SHIFT)))

#endif
