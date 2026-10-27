#include <nuvix/pgtable.h>
#include <nuvix/layout.h>
#include <nuvix/page.h>
#include <nuvix/memblock.h>
#include <nuvix/dtb.h>
#include <nuvix/ctypes.h>
#include <nuvix/irq.h>
#include <nuvix/tlb.h>
#include <nuvix/errno.h>
#include <nuvix/assert.h>
#include <nuvix/math.h>
#include <nuvix/string.h>
#include <asm/csr.h>

extern char __text_start[], __text_end[], __rodata_end[], __end[];

static paddr_t kernel_pgd;

static inline pt_entry_t *pgtable_ptr(paddr_t paddr)
{
    return (pt_entry_t *)__va(paddr);
}

static int pgtable_alloc_early(paddr_t *paddr)
{
    int ret = early_alloc(PAGE_SIZE, PAGE_SIZE, paddr);

    if (ret)
        return ret;
    refcount_set(&phys_to_page(*paddr)->refcount, 1);
    return 0;
}

static int pgtable_alloc_buddy(paddr_t *paddr)
{
    struct page *page = alloc_page();

    if (!page)
        return -ENOMEM;
    *paddr = page_to_phys(page);
    return 0;
}

static int (*pgtable_alloc)(paddr_t *paddr) = pgtable_alloc_early;

int alloc_pgtable(paddr_t *paddr)
{
    int ret = pgtable_alloc(paddr);

    if (ret)
        return ret;
    memset(pgtable_ptr(*paddr), 0, PAGE_SIZE);
    phys_to_page(*paddr)->flags |= PG_PGTABLE;
    return 0;
}

static int walk_pte(pt_entry_t *pgd, vaddr_t addr, pt_entry_t **pte)
{
    pt_entry_t entry = pt_entry_read(&pgd[PGD_INDEX(addr)]);
    pt_entry_t *pmd;

    if (!(entry & PTE_V))
        return -ENOENT;
    pmd = pgtable_ptr(pt_entry_paddr(entry)) + PMD_INDEX(addr);
    entry = pt_entry_read(pmd);
    if (!(entry & PTE_V))
        return -ENOENT;
    *pte = pgtable_ptr(pt_entry_paddr(entry)) + PTE_INDEX(addr);
    return 0;
}

static int walk_pte_create(pt_entry_t *pgd, vaddr_t addr, pt_entry_t **pte)
{
    pt_entry_t entry = pt_entry_read(&pgd[PGD_INDEX(addr)]);
    pt_entry_t *pmd;
    paddr_t paddr;
    int ret;

    if (!(entry & PTE_V)) {
        ret = alloc_pgtable(&paddr);
        if (ret)
            return ret;
        entry = pt_entry_make(paddr, PTE_TABLE);
        pt_entry_write(&pgd[PGD_INDEX(addr)], entry);
    }
    pmd = pgtable_ptr(pt_entry_paddr(entry)) + PMD_INDEX(addr);
    entry = pt_entry_read(pmd);
    if (!(entry & PTE_V)) {
        ret = alloc_pgtable(&paddr);
        if (ret)
            return ret;
        entry = pt_entry_make(paddr, PTE_TABLE);
        pt_entry_write(pmd, entry);
    }
    *pte = pgtable_ptr(pt_entry_paddr(entry)) + PTE_INDEX(addr);
    return 0;
}

static int map_page(pt_entry_t *pgd, vaddr_t addr, paddr_t paddr, uint64_t prot)
{
    pt_entry_t *pte;
    int ret = walk_pte_create(pgd, addr, &pte);

    if (ret)
        return ret;
    if (!pt_entry_none(pt_entry_read(pte)))
        return -EEXIST;
    pt_entry_write(pte, pt_entry_make(paddr, prot));
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
           (PT_ENTRIES - KERNEL_PGD_FIRST) * sizeof(pt_entry_t));
    *paddr = root;
    return 0;
}

void pgtable_free(paddr_t paddr)
{
    pt_entry_t *pgd = pgtable_ptr(paddr);
    struct page *page;

    for (unsigned int i = 0; i < KERNEL_PGD_FIRST; ++i) {
        pt_entry_t entry = pt_entry_read(&pgd[i]);
        pt_entry_t *pmd;
        paddr_t pmd_paddr;

        if (!(entry & PTE_V))
            continue;
        pt_entry_write(&pgd[i], 0);
        pmd_paddr = pt_entry_paddr(entry);
        pmd = pgtable_ptr(pmd_paddr);
        for (unsigned int j = 0; j < PT_ENTRIES; ++j) {
            pt_entry_t *pte;
            paddr_t pte_paddr;

            entry = pt_entry_read(&pmd[j]);
            if (!(entry & PTE_V))
                continue;
            pt_entry_write(&pmd[j], 0);
            pte_paddr = pt_entry_paddr(entry);
            pte = pgtable_ptr(pte_paddr);
            for (unsigned int k = 0; k < PT_ENTRIES; ++k) {
                entry = pt_entry_read(&pte[k]);
                if (!pt_entry_present(entry))
                    continue;
                pt_entry_write(&pte[k], 0);
                free_page(phys_to_page(pt_entry_paddr(entry)));
            }
            page = phys_to_page(pte_paddr);
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

struct page *pgtable_unmap_page(struct mm_struct *mm, vaddr_t addr)
{
    pt_entry_t *pte;
    pt_entry_t entry = 0;
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
    Assert(pt_entry_present(entry) && (entry & PTE_U),
           "unmap requires an ordinary user leaf\n");
    return phys_to_page(pt_entry_paddr(entry));
}

bool pgtable_protect_page(struct mm_struct *mm, vaddr_t addr,
                          unsigned long vm_flags)
{
    pt_entry_t *pte;
    pt_entry_t entry;
    uint64_t prot = vma_to_pte(vm_flags);
    unsigned long flags;

    Assert(mm->mmap_lock.owner == current,
           "page protection requires the mmap lock\n");
    spin_lock_irqsave(&mm->pgtable_lock, flags);
    if (!walk_pte(pgtable_ptr(mm->pgd), addr, &pte)) {
        entry = pt_entry_read(pte);
        if (pt_entry_present(entry)) {
            struct page *page = phys_to_page(pt_entry_paddr(entry));
            pt_entry_t value;

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
    pt_entry_t *pte;
    pt_entry_t entry;
    unsigned long flags;
    int ret;

    if (addr < UVADDR_START || addr >= UVADDR_END)
        return -EINVAL;
    spin_lock_irqsave(&mm->pgtable_lock, flags);
    ret = walk_pte(pgtable_ptr(mm->pgd), addr, &pte);
    if (!ret) {
        entry = pt_entry_read(pte);
        if (!pt_entry_valid(entry))
            ret = -ENOENT;
        else
            *paddr = pt_entry_paddr(entry) + (addr & (PAGE_SIZE - 1));
    }
    spin_unlock_irqrestore(&mm->pgtable_lock, flags);
    return ret;
}

static bool range_no_map(paddr_t paddr, size_t size)
{
    for (unsigned int i = 0; i < memblock.nr_reserved; ++i) {
        const struct memblock_region *region = &memblock.reserved[i];

        if (region->no_map && region->base < paddr + size &&
            paddr < region->base + region->size)
            return true;
    }
    return false;
}

static int create_kpgd(paddr_t *root)
{
    pt_entry_t *pgd;
    int ret = alloc_pgtable(root);

    if (ret)
        return ret;
    pgd = pgtable_ptr(*root);
    for (unsigned int i = KERNEL_PGD_FIRST; i < PT_ENTRIES; ++i) {
        paddr_t paddr;

        ret = alloc_pgtable(&paddr);
        if (ret)
            return ret;
        pt_entry_write(&pgd[i], pt_entry_make(paddr, PTE_TABLE));
    }
    return 0;
}

static int map_ram(pt_entry_t *pgd)
{
    paddr_t text_start = __pa((vaddr_t)__text_start);
    paddr_t text_end = __pa((vaddr_t)__text_end);
    paddr_t ro_end = __pa((vaddr_t)__rodata_end);

    for (unsigned long pfn = min_pfn; pfn < max_pfn; ++pfn) {
        paddr_t paddr = (paddr_t)pfn << PAGE_SHIFT;
        uint64_t prot = PTE_KERNEL_RW;
        int ret;

        if (range_no_map(paddr, PAGE_SIZE))
            continue;
        if (paddr >= text_start && paddr < ro_end)
            prot = paddr < text_end ? PTE_KERNEL_RX : PTE_KERNEL_RO;
        ret = map_page(pgd, __va(paddr), paddr, prot);
        if (ret)
            return ret;
    }
    return 0;
}

int pgtable_init(struct dtb *dtb)
{
    paddr_t image_end = __pa((vaddr_t)__end);
    paddr_t root, dtb_start, dtb_end;
    struct page *pages;
    const void *blob;
    int ret;

    dtb_start = ALIGN_DOWN(dtb->paddr, (paddr_t)PAGE_SIZE);
    dtb_end = ALIGN_UP(dtb->paddr + dtb->size, (paddr_t)PAGE_SIZE);
    if (range_no_map(KERNEL_PBASE, image_end - KERNEL_PBASE) ||
        range_no_map(dtb_start, dtb_end - dtb_start))
        return -EACCES;

    ret = create_kpgd(&root);
    if (ret)
        return ret;
    ret = map_ram(pgtable_ptr(root));
    if (ret)
        return ret;

    pages = (struct page *)__va((uintptr_t)mem_map);
    blob = (const void *)__va(dtb->paddr);
    refcount_inc(&phys_to_page(root)->refcount);

    csr_write(satp, SATP_SV39 | (root >> PAGE_SHIFT));
    local_flush_tlb_all();
    mem_map = pages;
    dtb->blob = blob;
    kernel_pgd = root;
    return 0;
}

void pgtable_use_buddy(void)
{
    pgtable_alloc = pgtable_alloc_buddy;
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
    pt_entry_t *pgd = pgtable_ptr(src->pgd);
    int ret = 0;

    Assert(src->mmap_lock.owner == current,
           "page table clone requires the source mmap lock\n");
    for (unsigned int i = 0; i < KERNEL_PGD_FIRST; ++i) {
        pt_entry_t entry = pt_entry_read(&pgd[i]);
        pt_entry_t *pmd;

        if (!(entry & PTE_V))
            continue;
        pmd = pgtable_ptr(pt_entry_paddr(entry));
        for (unsigned int j = 0; j < PT_ENTRIES; ++j) {
            pt_entry_t *pte;

            entry = pt_entry_read(&pmd[j]);
            if (!(entry & PTE_V))
                continue;
            pte = pgtable_ptr(pt_entry_paddr(entry));
            for (unsigned int k = 0; k < PT_ENTRIES; ++k) {
                vaddr_t addr = ((vaddr_t)i << PGD_SHIFT) |
                               ((vaddr_t)j << PMD_SHIFT) |
                               ((vaddr_t)k << PAGE_SHIFT);
                paddr_t paddr;
                unsigned long flags;
                pt_entry_t value;

                spin_lock_irqsave(&src->pgtable_lock, flags);
                entry = pt_entry_read(&pte[k]);
                if (!pt_entry_present(entry)) {
                    spin_unlock_irqrestore(&src->pgtable_lock, flags);
                    continue;
                }
                do {
                    value = entry & ~PTE_W;
                } while (!try_cmpxchg(&pte[k], &entry, value));
                spin_unlock_irqrestore(&src->pgtable_lock, flags);
                paddr = pt_entry_paddr(value);
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
