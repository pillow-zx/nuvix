#include <nuvix/pgtable.h>
#include <nuvix/layout.h>
#include <nuvix/page.h>
#include <nuvix/memblock.h>
#include <nuvix/dtb.h>
#include <nuvix/ctypes.h>
#include <nuvix/irq.h>
#include <nuvix/tlb.h>
#include <nuvix/errno.h>
#include <nuvix/math.h>
#include <nuvix/string.h>
#include <asm/csr.h>

extern char __text_start[], __text_end[], __rodata_end[], __end[];

static inline pt_entry_t *pgtable_ptr(paddr_t paddr)
{
    return (pt_entry_t *)__va(paddr);
}

static void mark_table(paddr_t paddr)
{
    struct page *page = phys_to_page(paddr);

    page->flags |= PG_PGTABLE;
    refcount_set(&page->refcount, 1);
}

static int alloc_pgtable(paddr_t *paddr)
{
    int ret = early_alloc(PAGE_SIZE, PAGE_SIZE, paddr);

    if (ret)
        return ret;
    memset(pgtable_ptr(*paddr), 0, PAGE_SIZE);
    mark_table(*paddr);
    return 0;
}

static __unused int walk_pte(pt_entry_t *pgd, vaddr_t addr, pt_entry_t **pte)
{
    pt_entry_t entry = pt_entry_read(&pgd[PGD_INDEX(addr)]);
    pt_entry_t *pmd;

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
    if (pt_entry_read(pte) & PTE_V)
        return -EEXIST;
    pt_entry_write(pte, pt_entry_make(paddr, prot));
    return 0;
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
#ifdef CONFIG_UART
    ret = map_page(pgtable_ptr(root), UART_VBASE, UART_PBASE, PTE_KERNEL_RW);
    if (ret)
        return ret;
#endif

    pages = (struct page *)__va((uintptr_t)mem_map);
    blob = (const void *)__va(dtb->paddr);
    refcount_inc(&phys_to_page(root)->refcount);

    csr_write(satp, SATP_SV39 | (root >> PAGE_SHIFT));
    local_flush_tlb_all();
    mem_map = pages;
    dtb->blob = blob;
    return 0;
}
