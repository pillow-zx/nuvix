#ifndef _NUVIX_PGTABLE_H
#define _NUVIX_PGTABLE_H

#include <asm/pgtable.h>
#include <nuvix/cleanup.h>

struct dtb;

/*
 * Called once after page_init(), with valid DTB and boot-accessible RAM.
 * Failed builds leave allocations reserved.
 */
int pgtable_init(struct dtb *dtb);

/* Boot-only, once after buddy_init() succeeds, before starting SMP. */
void pgtable_use_buddy(void);

/*
 * Boot-only after pgtable_use_buddy(), before SMP or user roots are created.
 * Maps page-aligned MMIO into the IOREMAP window of the permanent kernel root.
 * Caller owns the virtual range. Failure leaves partial mappings in place;
 * mappings and page tables remain permanent. Completes a local TLB flush.
 */
int pgtable_map_io(vaddr_t vaddr, paddr_t paddr, size_t size);

/*
 * After page_init(). Allocates one zeroed page with PG_PGTABLE and one ownership
 * reference. Returns 0 and a page-aligned physical address, or a negative errno
 * without changing *paddr. Early pages remain reserved; after the backend switch
 * pages come from buddy and may be released with pgtable_free().
 */
int alloc_pgtable(paddr_t *paddr);

/*
 * Final destruction of a buddy-backed page returned by alloc_pgtable(). Requires
 * exclusive ownership, only the ownership reference, completed detachment and
 * necessary TLB synchronization, and no hardware or software users. Call outside
 * short locks. Reserved boot pages must not be passed here.
 */
void pgtable_free(paddr_t paddr);

/*
 * After pgtable_init() and pgtable_use_buddy(). Allocates an independent root
 * with an empty user half and shared kernel entries, holding one ownership
 * reference. Returns 0 or a negative errno without changing *paddr on failure.
 */
int alloc_pgd(paddr_t *paddr);

/*
 * Releases only a root returned by alloc_pgd(); the user half must be empty.
 * Requires the same exclusive ownership and TLB synchronization as
 * pgtable_free(). Shared kernel tables remain permanently reserved.
 */
void free_pgd(paddr_t paddr);

DECLARE_FREE(pgd, paddr_t, if (_T) free_pgd(_T));

#endif /* ifndef _NUVIX_PGTABLE_H */
