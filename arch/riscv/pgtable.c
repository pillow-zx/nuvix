#include <nuvix/pgtable.h>
#include <nuvix/layout.h>
#include <nuvix/page.h>
#include <nuvix/ctypes.h>
#include <nuvix/irq.h>
#include <nuvix/tlb.h>
#include <nuvix/errno.h>
#include <nuvix/assert.h>
#include <nuvix/string.h>
#include <asm/csr.h>

static inline pte_t *pgtable_ptr(paddr_t paddr)
{
    return (pte_t *)__va(paddr);
}

int alloc_pgtable(paddr_t *paddr)
{
    struct page *page = alloc_page();

    if (!page)
        return -ENOMEM;
    *paddr = page_to_phys(page);
    memset(pgtable_ptr(*paddr), 0, PAGE_SIZE);
    page->flags |= PG_PGTABLE;
    return 0;
}

static int walk_pte(pte_t *pgd, vaddr_t addr, pte_t **pte)
{
    pte_t entry = pte_read(&pgd[PGD_INDEX(addr)]);
    pte_t *pmd;

    if (!(entry & PTE_V))
        return -ENOENT;
    pmd = pgtable_ptr(pte_paddr(entry)) + PMD_INDEX(addr);
    entry = pte_read(pmd);
    if (!(entry & PTE_V))
        return -ENOENT;
    *pte = pgtable_ptr(pte_paddr(entry)) + PTE_INDEX(addr);
    return 0;
}

static int walk_pte_create(pte_t *pgd, vaddr_t addr, pte_t **pte)
{
    pte_t entry = pte_read(&pgd[PGD_INDEX(addr)]);
    pte_t *pmd;
    paddr_t paddr;
    int ret;

    if (!(entry & PTE_V)) {
        ret = alloc_pgtable(&paddr);
        if (ret)
            return ret;
        entry = pte_make(paddr, PTE_TABLE);
        pte_write(&pgd[PGD_INDEX(addr)], entry);
    }
    pmd = pgtable_ptr(pte_paddr(entry)) + PMD_INDEX(addr);
    entry = pte_read(pmd);
    if (!(entry & PTE_V)) {
        ret = alloc_pgtable(&paddr);
        if (ret)
            return ret;
        entry = pte_make(paddr, PTE_TABLE);
        pte_write(pmd, entry);
    }
    *pte = pgtable_ptr(pte_paddr(entry)) + PTE_INDEX(addr);
    return 0;
}

static int map_page(pte_t *pgd, vaddr_t addr, paddr_t paddr, uint64_t prot)
{
    pte_t *pte;
    int ret = walk_pte_create(pgd, addr, &pte);

    if (ret)
        return ret;
    if (!pte_none(pte_read(pte)))
        return -EEXIST;
    pte_write(pte, pte_make(paddr, prot));
    return 0;
}

int alloc_pgd(paddr_t *paddr)
{
    paddr_t root;
    int ret = alloc_pgtable(&root);

    if (ret)
        return ret;
    memcpy(pgtable_ptr(root) + KERNEL_PGD_FIRST,
           pgtable_ptr(kernel_pgd) + KERNEL_PGD_FIRST,
           (PT_ENTRIES - KERNEL_PGD_FIRST) * sizeof(pte_t));
    *paddr = root;
    return 0;
}

void pgtable_free(paddr_t paddr)
{
    pte_t *pgd = pgtable_ptr(paddr);
    struct page *page;

    for (unsigned int i = 0; i < KERNEL_PGD_FIRST; ++i) {
        pte_t entry = pte_read(&pgd[i]);
        pte_t *pmd;
        paddr_t pmd_paddr;

        if (!(entry & PTE_V))
            continue;
        pte_write(&pgd[i], 0);
        pmd_paddr = pte_paddr(entry);
        pmd = pgtable_ptr(pmd_paddr);
        for (unsigned int j = 0; j < PT_ENTRIES; ++j) {
            pte_t *pte;
            paddr_t addr;

            entry = pte_read(&pmd[j]);
            if (!(entry & PTE_V))
                continue;
            pte_write(&pmd[j], 0);
            addr = pte_paddr(entry);
            pte = pgtable_ptr(addr);
            for (unsigned int k = 0; k < PT_ENTRIES; ++k) {
                entry = pte_read(&pte[k]);
                if (!pte_present(entry))
                    continue;
                pte_write(&pte[k], 0);
                free_page(phys_to_page(pte_paddr(entry)));
            }
            page = phys_to_page(addr);
            Assert(
                page->flags == (PG_ALLOCATED | PG_PGTABLE) &&
                    page->order == 0 && refcount_read(&page->refcount) == 1,
                "page table free requires exclusive ownership of a buddy page\n");
            page->flags &= ~PG_PGTABLE;
            free_page(page);
        }
        page = phys_to_page(pmd_paddr);
        Assert(
            page->flags == (PG_ALLOCATED | PG_PGTABLE) && page->order == 0 &&
                refcount_read(&page->refcount) == 1,
            "page table free requires exclusive ownership of a buddy page\n");
        page->flags &= ~PG_PGTABLE;
        free_page(page);
    }
    page = phys_to_page(paddr);
    Assert(page->flags == (PG_ALLOCATED | PG_PGTABLE) && page->order == 0 &&
               refcount_read(&page->refcount) == 1,
           "page table free requires exclusive ownership of a buddy page\n");
    page->flags &= ~PG_PGTABLE;
    free_page(page);
}

void pgtable_switch(struct mm_struct *mm)
{
    paddr_t pgd = mm ? mm->pgd : kernel_pgd;

    csr_write(satp, SATP_SV39 | (pgd >> PAGE_SHIFT));
    local_flush_tlb_all();
}

int pgtable_map_page(struct mm_struct *mm, vaddr_t addr, struct page *page,
                     unsigned long vm_flags)
{
    uint64_t prot;
    unsigned long flags;
    int ret;

    might_sleep();
    if (addr < UVADDR_START || addr >= UVADDR_END || (addr & (PAGE_SIZE - 1)) ||
        (vm_flags & ~(VM_READ | VM_WRITE | VM_EXEC)))
        return -EINVAL;
    prot = vma_to_pte(vm_flags);
    /* Code must be synchronized before any CPU can observe the executable PTE. */
    if (prot & PTE_X)
        flush_icache_all();
    spin_lock_irqsave(&mm->pgtable_lock, flags);
    ret = map_page(pgtable_ptr(mm->pgd), addr, page_to_phys(page), prot);
    spin_unlock_irqrestore(&mm->pgtable_lock, flags);
    if (!ret)
        flush_tlb_all();
    return ret;
}

int pgtable_move(struct mm_struct *mm, vaddr_t old, vaddr_t new, size_t len)
{
    pte_t *pgd = pgtable_ptr(mm->pgd);
    unsigned long flags;

    might_sleep();
    Assert(mm->mmap_lock.owner == current && len &&
               !((old | new | len) & (PAGE_SIZE - 1)) &&
               (old + len <= new || new + len <= old),
           "page table move requires locked, aligned, disjoint ranges\n");
    for (size_t offset = 0; offset < len; offset += PAGE_SIZE) {
        pte_t *src, *dst;
        int ret;

        spin_lock_irqsave(&mm->pgtable_lock, flags);
        ret = walk_pte(pgd, old + offset, &src);
        if (ret || !pte_present(pte_read(src))) {
            spin_unlock_irqrestore(&mm->pgtable_lock, flags);
            continue;
        }
        ret = walk_pte_create(pgd, new + offset, &dst);
        if (!ret)
            Assert(pte_none(pte_read(dst)),
                   "page table move requires an empty destination\n");
        spin_unlock_irqrestore(&mm->pgtable_lock, flags);
        if (ret)
            return ret;
    }

    flush_icache_all();
    for (size_t offset = 0; offset < len; offset += PAGE_SIZE) {
        pte_t *src, *dst;
        int ret;

        spin_lock_irqsave(&mm->pgtable_lock, flags);
        ret = walk_pte(pgd, old + offset, &src);
        if (ret || !pte_present(pte_read(src))) {
            spin_unlock_irqrestore(&mm->pgtable_lock, flags);
            continue;
        }
        ret = walk_pte(pgd, new + offset, &dst);
        Assert(!ret && pte_none(pte_read(dst)),
               "page table move requires a prepared destination\n");
        pte_write(dst, xchg(src, 0));
        spin_unlock_irqrestore(&mm->pgtable_lock, flags);
    }
    flush_tlb_all();
    return 0;
}

struct page *pgtable_unmap_page(struct mm_struct *mm, vaddr_t addr)
{
    pte_t *pte;
    pte_t entry = 0;
    unsigned long flags;

    Assert(addr >= UVADDR_START && addr < UVADDR_END &&
               !(addr & (PAGE_SIZE - 1)),
           "unmap requires a page-aligned user address\n");
    spin_lock_irqsave(&mm->pgtable_lock, flags);
    if (!walk_pte(pgtable_ptr(mm->pgd), addr, &pte))
        entry = xchg(pte, 0);
    spin_unlock_irqrestore(&mm->pgtable_lock, flags);

    if (!entry)
        return NULL;
    Assert(pte_present(entry) && (entry & PTE_U),
           "unmap requires an ordinary user leaf\n");
    return phys_to_page(pte_paddr(entry));
}

bool pgtable_protect_page(struct mm_struct *mm, vaddr_t addr,
                          unsigned long vm_flags)
{
    pte_t *pte;
    pte_t entry;
    uint64_t prot = vma_to_pte(vm_flags);
    unsigned long flags;

    Assert(mm->mmap_lock.owner == current,
           "page protection requires the mmap lock\n");
    spin_lock_irqsave(&mm->pgtable_lock, flags);
    if (!walk_pte(pgtable_ptr(mm->pgd), addr, &pte)) {
        entry = pte_read(pte);
        if (pte_present(entry)) {
            struct page *page = phys_to_page(pte_paddr(entry));
            pte_t value;

            if (refcount_read(&page->refcount) != 1)
                prot &= ~PTE_W;
            do {
                value = (entry & ~(PTE_V | PTE_LEAF | PTE_NONE)) | prot;
            } while (!try_cmpxchg(pte, &entry, value));
            spin_unlock_irqrestore(&mm->pgtable_lock, flags);
            return true;
        }
    }
    spin_unlock_irqrestore(&mm->pgtable_lock, flags);
    return false;
}

int pgtable_lookup(struct mm_struct *mm, vaddr_t addr, paddr_t *paddr)
{
    pte_t *pte;
    pte_t entry;
    unsigned long flags;
    int ret;

    if (addr < UVADDR_START || addr >= UVADDR_END)
        return -EINVAL;
    spin_lock_irqsave(&mm->pgtable_lock, flags);
    ret = walk_pte(pgtable_ptr(mm->pgd), addr, &pte);
    if (!ret) {
        entry = pte_read(pte);
        if (!pte_valid(entry))
            ret = -ENOENT;
        else
            *paddr = pte_paddr(entry) + (addr & (PAGE_SIZE - 1));
    }
    spin_unlock_irqrestore(&mm->pgtable_lock, flags);
    return ret;
}

int pgtable_map_io(vaddr_t vaddr, paddr_t paddr, size_t size)
{
    int ret = 0;

    if (!size || ((vaddr | paddr | size) & (PAGE_SIZE - 1)) ||
        vaddr < IOREMAP_VBASE)
        return -EINVAL;
    if (size > UINT64_MAX - vaddr || size > UINT64_MAX - paddr)
        return -EOVERFLOW;
    for (size_t offset = 0; offset < size; offset += PAGE_SIZE) {
        ret = map_page(pgtable_ptr(kernel_pgd), vaddr + offset, paddr + offset,
                       PTE_KERNEL_RW);
        if (ret)
            break;
    }
    local_flush_tlb_all();
    return ret;
}

int pgtable_clone(struct mm_struct *dst, struct mm_struct *src)
{
    pte_t *pgd = pgtable_ptr(src->pgd);
    int ret = 0;

    Assert(src->mmap_lock.owner == current,
           "page table clone requires the source mmap lock\n");
    for (unsigned int i = 0; i < KERNEL_PGD_FIRST; ++i) {
        pte_t entry = pte_read(&pgd[i]);
        pte_t *pmd;

        if (!(entry & PTE_V))
            continue;
        pmd = pgtable_ptr(pte_paddr(entry));
        for (unsigned int j = 0; j < PT_ENTRIES; ++j) {
            pte_t *pte;

            entry = pte_read(&pmd[j]);
            if (!(entry & PTE_V))
                continue;
            pte = pgtable_ptr(pte_paddr(entry));
            for (unsigned int k = 0; k < PT_ENTRIES; ++k) {
                vaddr_t addr = ((vaddr_t)i << PGD_SHIFT) |
                               ((vaddr_t)j << PMD_SHIFT) |
                               ((vaddr_t)k << PAGE_SHIFT);
                paddr_t paddr;
                unsigned long flags;
                pte_t value;

                spin_lock_irqsave(&src->pgtable_lock, flags);
                entry = pte_read(&pte[k]);
                if (!pte_present(entry)) {
                    spin_unlock_irqrestore(&src->pgtable_lock, flags);
                    continue;
                }
                do {
                    value = entry & ~PTE_W;
                } while (!try_cmpxchg(&pte[k], &entry, value));
                spin_unlock_irqrestore(&src->pgtable_lock, flags);
                paddr = pte_paddr(value);
                ret = map_page(pgtable_ptr(dst->pgd), addr, paddr,
                               value & (BIT(PTE_PPN_SHIFT) - 1));
                if (ret)
                    goto out;
                refcount_inc(&phys_to_page(paddr)->refcount);
            }
        }
    }
out:
    flush_tlb_all();
    return ret;
}
