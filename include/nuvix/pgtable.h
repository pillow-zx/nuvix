#ifndef _NUVIX_PGTABLE_H
#define _NUVIX_PGTABLE_H

#include <asm/pgtable.h>

struct dtb;

/*
 * Called once after page_init(), with valid DTB and boot-accessible RAM.
 * Failed builds leave allocations reserved.
 */
int pgtable_init(struct dtb *dtb);

/* Boot-only, once after buddy_init() succeeds, before starting SMP. */
void pgtable_use_buddy(void);

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

#endif /* ifndef _NUVIX_PGTABLE_H */
