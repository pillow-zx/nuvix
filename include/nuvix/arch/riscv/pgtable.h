#ifndef _NUVIX_RISCV_PGTABLE_H
#define _NUVIX_RISCV_PGTABLE_H

#include <nuvix/arch/riscv/layout.h>

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

#define PTE_TABLE        PTE_V
#define PTE_LEAF         (PTE_R | PTE_W | PTE_X)

#define PTE_KERNEL_RO    (PTE_V | PTE_R | PTE_A)
#define PTE_KERNEL_RW    (PTE_V | PTE_R | PTE_A | PTE_W | PTE_D)
#define PTE_KERNEL_RX    (PTE_V | PTE_R | PTE_A | PTE_X)

#define PTE_BOOT_RWX     (PTE_KERNEL_RW | PTE_X)

#define SATP_SV39        0x8000000000000000

#ifndef __ASSEMBLER__
#include <nuvix/ctypes.h>

typedef uint64_t pt_entry_t;

static inline pt_entry_t pt_entry_make(paddr_t paddr, uint64_t flags)
{
    return ((paddr >> PAGE_SHIFT) << PTE_PPN_SHIFT) | flags;
}

static inline paddr_t pt_entry_paddr(pt_entry_t entry)
{
    return ((entry >> PTE_PPN_SHIFT) & ((1ULL << PTE_PPN_BITS) - 1))
           << PAGE_SHIFT;
}

static inline pt_entry_t pt_entry_read(const pt_entry_t *entry)
{
    return __atomic_load_n(entry, __ATOMIC_RELAXED);
}

static inline void pt_entry_write(pt_entry_t *entry, pt_entry_t value)
{
    __atomic_store_n(entry, value, __ATOMIC_RELEASE);
}
#endif

#endif /* _NUVIX_RISCV_PGTABLE_H */
