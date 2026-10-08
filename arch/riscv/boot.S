#include <asm/pgtable.h>
#include <asm/irq.h>
#include <asm/smp.h>

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

    li          t0, 0x0000000080000000
    srli        t0, t0, PAGE_SHIFT
    slli        t0, t0, PTE_PPN_SHIFT
    ori         t0, t0, PTE_BOOT_RWX
    li          t1, PGD_INDEX(KERNEL_PBASE) * PT_ENTRY_SIZE
    add         t1, s2, t1
    sd          t0, 0(t1)

    li          t1, PGD_INDEX(KERNEL_VBASE) * PT_ENTRY_SIZE
    add         t1, s2, t1
    sd          t0, 0(t1)

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
    lla         t0, __alltraps
    csrw        stvec, t0

    lla         tp, boot_task
    lla         a0, boot_stack
    call        task_boot_init

    mv          a0, s0
    mv          a1, s1
    call        kernel_main
.Lpark:
    wfi
    j           .Lpark

    .balign 4
    .globl smp_entry
    .type smp_entry, @function
smp_entry:
    csrw        sie, zero
    csrci       sstatus, SSTATUS_SIE
    csrw        sscratch, zero
    lla         t0, .Lpark
    csrw        stvec, t0

    fence       r, rw
    ld          s0, CPU_BOOT_TASK(a1)
    ld          s1, CPU_BOOT_STACK(a1)
    ld          s2, CPU_BOOT_SATP(a1)
    lla         t0, boot_pgd
    srli        t0, t0, PAGE_SHIFT
    li          t2, SATP_SV39
    or          t0, t0, t2
    lla         t1, .Lsmp_virtual
    li          t2, (KERNEL_VBASE - KERNEL_PBASE)
    add         t1, t1, t2
    sfence.vma  zero, zero
    csrw        satp, t0
    sfence.vma  zero, zero
    jr          t1

.Lsmp_virtual:
    /* The final root maps stacks that may lie outside the boot mapping. */
    lla         t0, .Lpark
    csrw        stvec, t0
    csrw        satp, s2
    sfence.vma  zero, zero
    fence.i
    mv          tp, s0
    mv          sp, s1
    lla         t0, __alltraps
    csrw        stvec, t0
    call        smp_main
    j           .Lpark
    .size smp_entry, . - smp_entry

    .section .bss.boot, "aw", @nobits
    .balign PAGE_SIZE
boot_pgd:
    .skip PAGE_SIZE
    .balign 16
boot_stack:
    .skip BOOT_STACK_SIZE
boot_stack_top:
