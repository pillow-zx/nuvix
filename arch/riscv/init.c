#include <nuvix/pgtable.h>
#include <nuvix/layout.h>
#include <nuvix/page.h>
#include <nuvix/memblock.h>
#include <nuvix/dtb.h>
#include <nuvix/errno.h>
#include <nuvix/math.h>
#include <nuvix/string.h>

extern char __text_start[], __text_end[], __rodata_end[], __end[];

paddr_t kernel_pgd;

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

static int boot_alloc_pgtable(paddr_t *paddr)
{
    int ret = early_alloc(PAGE_SIZE, PAGE_SIZE, paddr);

    if (ret)
        return ret;
    memset((pte_t *)(*paddr), 0, PAGE_SIZE);
    refcount_set(&phys_to_page(*paddr)->refcount, 1);
    phys_to_page(*paddr)->flags |= PG_PGTABLE;
    return 0;
}

static int boot_map_page(pte_t *pgd, vaddr_t addr, paddr_t paddr, uint64_t prot)
{
    pte_t *pmd = (pte_t *)(pte_paddr(pgd[PGD_INDEX(addr)])) + PMD_INDEX(addr);
    pte_t *pte;
    paddr_t table;
    int ret;

    if (!(*pmd & PTE_V)) {
        ret = boot_alloc_pgtable(&table);
        if (ret)
            return ret;
        *pmd = pte_make(table, PTE_TABLE);
    }
    pte = (pte_t *)(pte_paddr(*pmd)) + PTE_INDEX(addr);
    *pte = pte_make(paddr, prot);
    return 0;
}

static int boot_create_kpgd(paddr_t *root)
{
    pte_t *pgd;
    int ret = boot_alloc_pgtable(root);

    if (ret)
        return ret;
    pgd = (pte_t *)(*root);
    for (unsigned int i = KERNEL_PGD_FIRST; i < PT_ENTRIES; ++i) {
        paddr_t paddr;

        ret = boot_alloc_pgtable(&paddr);
        if (ret)
            return ret;
        pte_write(&pgd[i], pte_make(paddr, PTE_TABLE));
    }
    return 0;
}

static int boot_map_ram(pte_t *pgd)
{
    paddr_t text_start = (uintptr_t)__text_start;
    paddr_t text_end = (uintptr_t)__text_end;
    paddr_t ro_end = (uintptr_t)__rodata_end;

    for (unsigned long pfn = min_pfn; pfn < max_pfn; ++pfn) {
        paddr_t paddr = (paddr_t)pfn << PAGE_SHIFT;
        uint64_t prot = PTE_KERNEL_RW;
        int ret;

        if (range_no_map(paddr, PAGE_SIZE))
            continue;
        if (paddr >= text_start && paddr < ro_end)
            prot = paddr < text_end ? PTE_KERNEL_RX : PTE_KERNEL_RO;
        ret = boot_map_page(pgd, __va(paddr), paddr, prot);
        if (ret)
            return ret;
    }
    return 0;
}

int pgtable_init(paddr_t *paddr)
{
    paddr_t image_end = (uintptr_t)__end;
    paddr_t root, dtb_start, dtb_end;
    int ret;

    dtb_start = ALIGN_DOWN(dtb.paddr, (paddr_t)PAGE_SIZE);
    dtb_end = ALIGN_UP(dtb.paddr + dtb.size, (paddr_t)PAGE_SIZE);
    if (range_no_map(KERNEL_PBASE, image_end - KERNEL_PBASE) ||
        range_no_map(dtb_start, dtb_end - dtb_start))
        return -EACCES;

    ret = boot_create_kpgd(&root);
    if (ret)
        return ret;
    ret = boot_map_ram((pte_t *)(root));
    if (ret)
        return ret;

    refcount_inc(&phys_to_page(root)->refcount);
    kernel_pgd = root;
    *paddr = root;
    return 0;
}
