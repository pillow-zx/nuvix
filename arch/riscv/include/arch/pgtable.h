#ifndef _CUTEOS_ARCH_RISCV_PGTABLE_H
#define _CUTEOS_ARCH_RISCV_PGTABLE_H

#include <kernel/compiler.h>
#include <kernel/types.h>
#include <arch/page.h>
#include <asm/csr.h>
#include <asm/pte.h>

typedef pte_t pgprot_t;

void pagetable_use_buddy(void);

/** Activate a page table and invalidate stale local translations. */
void activate_pgroot(uintptr_t satp);

__must_check
pte_t *current_pt(void);

__must_check
pte_t *kernel_pt(void);

__must_check
uintptr_t kenrel_pgroot(void);

__must_check __nonnull(1)
pte_t *pgtable_lookup(pte_t *root, uintptr_t va);

__must_check __nonnull(1)
int map_page(pte_t *root, uintptr_t va, uintptr_t pa, uint64_t perm);

__must_check __const
static inline  pgprot_t pgprot_user(bool read, bool write, bool exec)
{
	pgprot_t flags = PTE_V | PTE_U | PTE_A | PTE_D;

	if (read)
		flags |= PTE_R;
	if (write)
		flags |= PTE_R | PTE_W;
	if (exec)
		flags |= PTE_X;

	return flags;
}

__must_check __const
static inline pgprot_t
pgprot_kernel(bool read, bool write, bool exec)
{
	pgprot_t flags = PTE_V | PTE_G | PTE_A | PTE_D;

	if (read)
		flags |= PTE_R;
	if (write)
		flags |= PTE_R | PTE_W;
	if (exec)
		flags |= PTE_X;

	return flags;
}

__must_check __const
static inline bool pte_is_present(pte_t pte)
{
	return asm_pte_present(pte);
}

__must_check __pure
static inline  bool pte_is_user_page(pte_t pte)
{
	return asm_pte_user_page(pte);
}

__must_check __pure
static inline bool pte_allows_user_read(pte_t pte)
{
	return pte_is_present(pte) && (pte & PTE_U) && (pte & PTE_R);
}

__must_check __pure
static inline bool pte_allows_user_write(pte_t pte)
{
	return pte_is_present(pte) && (pte & PTE_U) && (pte & PTE_W);
}

__must_check __pure
static inline bool pte_allows_user_exec(pte_t pte)
{
	return pte_is_present(pte) && (pte & PTE_U) && (pte & PTE_X);
}

__must_check __pure
static inline paddr_t pte_phys_addr(pte_t pte)
{
	return asm_pte_to_pa(pte);
}

__must_check __pure
static inline pgprot_t pte_leaf_prot(pte_t pte)
{
	return pte & MASK(PTE_PPN_SHIFT);
}

__must_check __pure
static inline pte_t pte_make(paddr_t pa, pgprot_t prot)
{
	return PA_TO_PTE(pa) | prot;
}

__nonnull(1)
static inline void pte_clear_present(pte_t *pte)
{
	*pte &= ~PTE_V;
}

__must_check __pure
static inline  uintptr_t pgtable_make_token(const pte_t *pgd)
{
	return SATP_MODE_SV39 | (__pa((uintptr_t)pgd) >> PAGE_SHIFT);
}

__always_inline
static inline void pgtable_activate_kpgtable(void)
{
	activate_pgroot(kenrel_pgroot());
}

__always_inline
static inline void flush_tlb_all(void)
{
	tlb_flush_all();
}

__always_inline
static inline void flush_tlb_page(uintptr_t va)
{
	tlb_flush_page(va);
}

#endif
