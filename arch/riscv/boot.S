#include <asm/pgtable.h>
#include <asm/irq.h>
#ifdef CONFIG_SMP
#include <asm/smp.h>
#endif

    .section .text.boot, "ax"
    .balign 4
    .globl _start
_start:
    csrw        sie, zero
    csrci       sstatus, SSTATUS_SIE
    csrw        sscratch, zero
    lla         t0, .Lpark
    csrw        stvec, t0

    lla         t0, __bss_start
    lla         t1, __bss_end
.Lclear_bss:
    bgeu        t0, t1, .Lbss_done
    sd          zero, 0(t0)
    addi        t0, t0, 8
    j           .Lclear_bss
.Lbss_done:
    lla         sp, boot_stack_top
    lla         tp, boot_task
    call        start_kernel

    srli        a0, a0, PAGE_SHIFT
    li          t0, SATP_SV39
    or          a0, a0, t0
    call        .Lenable_mmu

    li          t0, (KERNEL_VBASE - KERNEL_PBASE)
    add         sp, sp, t0
    add         tp, tp, t0
    lla         t0, __alltraps
    csrw        stvec, t0

    lla         a0, boot_stack
    call        task_boot_init
    call        module_init
    call        sched_task
    .balign 4
.Lpark:
    wfi
    j           .Lpark

    /* a0 is the final satp; the switch uses registers only. */
    .balign 4
.Lenable_mmu:
    li          t0, (KERNEL_VBASE - KERNEL_PBASE)
    add         ra, ra, t0
    lla         t1, .Lmmu_virtual
    add         t1, t1, t0
    csrw        stvec, t1
    sfence.vma  zero, zero
    /* Only high addresses are mapped: the next fetch faults to high stvec. */
    csrw        satp, a0
    .balign 4
.Lmmu_virtual:
    sfence.vma  zero, zero
    lla         t0, .Lpark
    csrw        stvec, t0
    fence.i
    ret

#ifdef CONFIG_SMP
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
    ld          a0, CPU_BOOT_SATP(a1)
    call        .Lenable_mmu

    mv          tp, s0
    mv          sp, s1
    lla         t0, __alltraps
    csrw        stvec, t0
    call        smp_main
    j           .Lpark
    .size smp_entry, . - smp_entry
#endif

    .section .bss.boot, "aw", @nobits
    .balign 16
boot_stack:
    .skip BOOT_STACK_SIZE
boot_stack_top:
