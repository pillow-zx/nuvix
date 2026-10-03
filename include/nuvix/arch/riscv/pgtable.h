#ifndef _NUVIX_RISCV_PGTABLE_H
#define _NUVIX_RISCV_PGTABLE_H

#include <nuvix/arch/riscv/layout.h>

#define PT_INDEX_BITS   9
#define PT_INDEX_MASK   ((1 << PT_INDEX_BITS) - 1)
#define PMD_SHIFT       (PAGE_SHIFT + PT_INDEX_BITS)
#define PMD_SIZE        (1 << PMD_SHIFT)
#define PGD_SHIFT       (PMD_SHIFT + PT_INDEX_BITS)
#define PGD_SIZE        (1 << PGD_SHIFT)

#define BOOT_IDMAP_PBASE (KERNEL_PBASE & ~(PGD_SIZE - 1))
#define BOOT_IDMAP_PEND  (BOOT_IDMAP_PBASE + PGD_SIZE)

#define PGD_INDEX(addr) (((addr) >> PGD_SHIFT) & PT_INDEX_MASK)
#define PMD_INDEX(addr) (((addr) >> PMD_SHIFT) & PT_INDEX_MASK)
#define PTE_INDEX(addr) (((addr) >> PAGE_SHIFT) & PT_INDEX_MASK)

#define PT_ENTRY_SIZE   8
#define PTE_PPN_SHIFT   10

#define PTE_V           (1 << 0)
#define PTE_R           (1 << 1)
#define PTE_W           (1 << 2)
#define PTE_X           (1 << 3)
#define PTE_U           (1 << 4)
#define PTE_G           (1 << 5)
#define PTE_A           (1 << 6)
#define PTE_D           (1 << 7)

#define PTE_KERNEL_RO   (PTE_V | PTE_R | PTE_A)
#define PTE_KERNEL_RW   (PTE_V | PTE_R | PTE_A | PTE_W | PTE_D)
#define PTE_KERNEL_RX   (PTE_V | PTE_R | PTE_A | PTE_X)

#define PTE_BOOT_RWX    (PTE_KERNEL_RW | PTE_X)

#define SATP_SV39       0x8000000000000000

#endif /* _NUVIX_RISCV_PGTABLE_H */
