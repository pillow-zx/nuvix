#ifndef _NUVIX_PGTABLE_H
#define _NUVIX_PGTABLE_H

#include <asm/pgtable.h>
#include <nuvix/cleanup.h>

struct page;

/*
 * After buddy_init(). Allocates one zeroed buddy page with PG_PGTABLE and one
 * ownership reference. Returns 0 and a page-aligned physical address, or a
 * negative errno without changing *paddr.
 */
int alloc_pgtable(paddr_t *paddr);

/*
 * After buddy_init(). Allocates an independent root with an empty user half
 * and shared kernel entries, holding one ownership reference. Returns 0 or a
 * negative errno without changing *paddr on failure.
 */
int alloc_pgd(paddr_t *paddr);

/*
 * Releases a root returned by alloc_pgd(), its user tables and one reference
 * per user leaf. User leaves must map ordinary order-0 allocations; no huge
 * pages. Requires exclusive ownership, only the root's ownership reference,
 * completed detachment and necessary TLB synchronization, and no CPU/software
 * users. Call outside short locks. Shared kernel tables are retained; reserved
 * boot roots must not be passed here.
 */
void pgtable_free(paddr_t paddr);
int pgtable_clone(struct mm_struct *dst, struct mm_struct *src);
int pgtable_move(struct mm_struct *mm, vaddr_t old, vaddr_t new, size_t len);

/*
 * Local CPU, IRQ off. Loads mm's root, or the permanent kernel root for NULL,
 * with ASID 0 and a complete local TLB flush. The caller holds mm alive while
 * loaded and skips this operation when the address space has not changed.
 */
void pgtable_switch(struct mm_struct *mm);

/*
 * After SMP is ready, in ordinary thread context with mm held alive and its
 * mmap_lock held. Maps one page-aligned user address with VM_READ/WRITE/EXEC.
 * Page must be a fully initialized ordinary order-0 allocation; success takes
 * one caller-owned reference without incrementing it. Failure keeps that
 * reference with the caller; empty intermediate tables may remain owned by mm.
 * No existing leaf is replaced (-EEXIST). Completes all-CPU TLB synchronization,
 * and synchronizes executable contents before publication. Does not create VMA
 * metadata. The page's contents must not change during executable publication.
 */
int pgtable_map_page(struct mm_struct *mm, vaddr_t addr, struct page *page,
                     unsigned long vm_flags);
struct page *pgtable_unmap_page(struct mm_struct *mm, vaddr_t addr);
bool pgtable_protect_page(struct mm_struct *mm, vaddr_t addr,
                          unsigned long vm_flags);
/*
 * mm held alive, outside its pgtable_lock. Returns a physical address including
 * the offset within a page, or -EINVAL/-ENOENT without changing *paddr.
 * The result is a snapshot, not a page reference; retain mmap_lock if using it
 * to access the mapped page while concurrent unmapping is possible.
 */
int pgtable_lookup(struct mm_struct *mm, vaddr_t addr, paddr_t *paddr);

/*
 * Called once with MMU off, after page_init(). Builds the permanent kernel
 * root with high mappings only; returns its physical address in *paddr.
 * Leaves the MMU off. Failed builds leave allocations reserved.
 */
int pgtable_init(paddr_t *paddr);

/*
 * Maps page-aligned [paddr, paddr + size) at vaddr in mm's root, or in the
 * permanent kernel root's IOREMAP window when mm is NULL. The kernel window is
 * boot-only before SMP or user roots, leaves partial mappings in place and
 * flushes locally. A user mm requires its mmap_lock, marks leaves PTE_IO,
 * removes the leaves it created when an allocation fails, and flushes all CPUs.
 */
int pgtable_map_io(struct mm_struct *mm, vaddr_t vaddr, paddr_t paddr,
                   size_t size, unsigned long vm_flags);

DECLARE_FREE(pgd, paddr_t, if (_T) pgtable_free(_T));

#endif /* ifndef _NUVIX_PGTABLE_H */
