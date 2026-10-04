#include <nuvix/arch/riscv/pgtable.h>
#include <nuvix/arch/riscv/irq.h>

    .section .text.boot, "ax"
    .balign 4
    .globl _start
_start:
    csrw        sie, zero
    csrci       sstatus, SSTATUS_SIE
    csrw        sscratch, zero
    lla         t0, .Lpark
    csrw        stvec, t0
    mv          s0, a0
    mv          s1, a1

    lla         t0, __bss_start
    lla         t1, __bss_end
.Lclear_bss:
    bgeu        t0, t1, .Lbss_done
    sd          zero, 0(t0)
    addi        t0, t0, 8
    j           .Lclear_bss
.Lbss_done:
    lla         sp, boot_stack_top
    lla         s2, boot_pgd

    li          t0, BOOT_IDMAP_PBASE
    srli        t0, t0, PAGE_SHIFT
    slli        t0, t0, PTE_PPN_SHIFT
    ori         t0, t0, PTE_BOOT_RWX
    li          t1, PGD_INDEX(KERNEL_PBASE) * PT_ENTRY_SIZE
    add         t1, s2, t1
    sd          t0, 0(t1)

    li          t1, PGD_INDEX(KERNEL_VBASE) * PT_ENTRY_SIZE
    add         t1, s2, t1
    sd          t0, 0(t1)

#ifdef CONFIG_UART
    lla         t0, boot_uart_pmd
    srli        t0, t0, PAGE_SHIFT
    slli        t0, t0, PTE_PPN_SHIFT
    ori         t0, t0, PTE_V
    li          t1, PGD_INDEX(UART_VBASE) * PT_ENTRY_SIZE
    add         t1, s2, t1
    sd          t0, 0(t1)

    lla         t0, boot_uart_pte
    srli        t0, t0, PAGE_SHIFT
    slli        t0, t0, PTE_PPN_SHIFT
    ori         t0, t0, PTE_V
    lla         t1, boot_uart_pmd
    li          t2, PMD_INDEX(UART_VBASE) * PT_ENTRY_SIZE
    add         t1, t1, t2
    sd          t0, 0(t1)

    li          t0, UART_PBASE
    srli        t0, t0, PAGE_SHIFT
    slli        t0, t0, PTE_PPN_SHIFT
    ori         t0, t0, PTE_KERNEL_RW
    lla         t1, boot_uart_pte
    li          t2, PTE_INDEX(UART_VBASE) * PT_ENTRY_SIZE
    add         t1, t1, t2
    sd          t0, 0(t1)
#endif

    li          s3, (KERNEL_VBASE - KERNEL_PBASE)
    lla         t1, .Lvirtual
    add         t1, t1, s3
    srli        t0, s2, PAGE_SHIFT
    li          t2, SATP_SV39
    or          t0, t0, t2
    sfence.vma  zero, zero
    csrw        satp, t0
    sfence.vma  zero, zero
    jr          t1

.Lvirtual:
    add         sp, sp, s3
    add         s2, s2, s3
    lla         t0, trap_entry
    csrw        stvec, t0

    mv          a0, s0
    mv          a1, s1
    call        kernel_main
.Lpark:
    wfi
    j           .Lpark

    .section .bss.boot, "aw", @nobits
    .balign PAGE_SIZE
boot_pgd:
    .skip PAGE_SIZE
#ifdef CONFIG_UART
boot_uart_pmd:
    .skip PAGE_SIZE
boot_uart_pte:
    .skip PAGE_SIZE
#endif
    .balign 16
boot_stack:
    .skip BOOT_STACK_SIZE
boot_stack_top:
