#ifndef _NUVIX_RISCV_PGTABLE_H
#define _NUVIX_RISCV_PGTABLE_H

#include <asm/layout.h>

#define PT_INDEX_BITS    9
#define PT_INDEX_MASK    ((1 << PT_INDEX_BITS) - 1)
#define PT_ENTRIES       (1 << PT_INDEX_BITS)
#define KERNEL_PGD_FIRST (PT_ENTRIES / 2)
#define PMD_SHIFT        (PAGE_SHIFT + PT_INDEX_BITS)
#define PMD_SIZE         (1 << PMD_SHIFT)
#define PGD_SHIFT        (PMD_SHIFT + PT_INDEX_BITS)
#define PGD_SIZE         (1 << PGD_SHIFT)

#define PGD_INDEX(addr)  (((addr) >> PGD_SHIFT) & PT_INDEX_MASK)
#define PMD_INDEX(addr)  (((addr) >> PMD_SHIFT) & PT_INDEX_MASK)
#define PTE_INDEX(addr)  (((addr) >> PAGE_SHIFT) & PT_INDEX_MASK)

#define PT_ENTRY_SIZE    8
#define PTE_PPN_SHIFT    10
#define PTE_PPN_BITS     44

#define PTE_V            (1 << 0)
#define PTE_R            (1 << 1)
#define PTE_W            (1 << 2)
#define PTE_X            (1 << 3)
#define PTE_U            (1 << 4)
#define PTE_G            (1 << 5)
#define PTE_A            (1 << 6)
#define PTE_D            (1 << 7)
#define PTE_NONE         (1 << 8)

#define PTE_TABLE        PTE_V
#define PTE_LEAF         (PTE_R | PTE_W | PTE_X)

#define PTE_KERNEL_RO    (PTE_V | PTE_R | PTE_A)
#define PTE_KERNEL_RW    (PTE_V | PTE_R | PTE_A | PTE_W | PTE_D)
#define PTE_KERNEL_RX    (PTE_V | PTE_R | PTE_A | PTE_X)

#define SATP_SV39        0x8000000000000000

#ifndef __ASSEMBLER__
#include <nuvix/atomic.h>
#include <nuvix/ctypes.h>
#include <nuvix/mm.h>

typedef uint64_t pte_t;

extern paddr_t kernel_pgd;

static inline uint64_t vma_to_pte(unsigned long vma_flags)
{
    uint64_t flags = PTE_V | PTE_U | PTE_A;

    if (!(vma_flags & (VM_READ | VM_WRITE | VM_EXEC)))
        return PTE_U | PTE_NONE;
    if (vma_flags & VM_READ)
        flags |= PTE_R;
    if (vma_flags & VM_WRITE)
        flags |= PTE_R | PTE_W | PTE_D;
    if (vma_flags & VM_EXEC)
        flags |= PTE_X;
    return flags;
}

static inline pte_t pte_make(paddr_t paddr, uint64_t flags)
{
    return ((paddr >> PAGE_SHIFT) << PTE_PPN_SHIFT) | flags;
}

static inline paddr_t pte_paddr(pte_t entry)
{
    return ((entry >> PTE_PPN_SHIFT) & ((1ULL << PTE_PPN_BITS) - 1))
           << PAGE_SHIFT;
}

static inline pte_t pte_read(const pte_t *entry)
{
    return load_relaxed(entry);
}

static inline bool pte_none(pte_t entry)
{
    return entry == 0;
}

static inline bool pte_valid(pte_t entry)
{
    return entry & PTE_V;
}

static inline bool pte_present(pte_t entry)
{
    return entry & (PTE_V | PTE_NONE);
}

static inline void pte_write(pte_t *entry, pte_t value)
{
    store_release(entry, value);
}
#endif

#endif /* _NUVIX_RISCV_PGTABLE_H */
