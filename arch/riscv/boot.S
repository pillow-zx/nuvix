/*
 * arch/riscv/boot.S - 内核入口 _start 与副核物理 trampoline
 */

#include <asm/asm_offsets.h>
#include <kernel/smp.h>

#define SSTATUS_SIE (1 << 1)
#define SATP_MODE_SV39_VAL 8

#define PTE_V   (1 << 0)
#define PTE_R   (1 << 1)
#define PTE_W   (1 << 2)
#define PTE_X   (1 << 3)
#define PTE_G   (1 << 5)

#define PTE_MEGA_DRAM_FLAGS  (PTE_V | PTE_R | PTE_W | PTE_X | PTE_G)
#define PTE_MEGA_MMIO_FLAGS  (PTE_V | PTE_R | PTE_W | PTE_G)

        .section .text.entry
        .globl _start
_start:

        mv      s0, a0
        bnez    s0, .park

        .option push
        .option norelax
        la      gp, __global_pointer$
        .option pop

        lla     sp, idle_stacks + 16 * 1024


        lla     t0, __bss_start
        lla     t1, _end
1:
        sd      zero, 0(t0)
        addi    t0, t0, 8
        bne     t0, t1, 1b


        lla     t0, tmp_root


        li      t1, (0x80000000 >> 2) | PTE_MEGA_DRAM_FLAGS
        sd      t1, (2*8)(t0)


        li      t2, 258*8
        add     t2, t0, t2
        sd      t1, 0(t2)


        li      t1, (0x00000000 >> 2) | PTE_MEGA_MMIO_FLAGS
        sd      t1, (0*8)(t0)


        lla     t0, tmp_root
        srli    t0, t0, 12
        li      t1, (8 << 60)
        or      t0, t0, t1
        csrw    satp, t0
        sfence.vma


        /* Jump to the high-half continuation, then re-establish gp and tp
         * there: after the jump the PC is high, so lla/la yield high-half
         * addresses. tp must be CPU 0's slot before any C runs. */
        lla     t0, 1f
        li      t1, 0xFFFFFFC000000000
        add     t0, t0, t1
        jr      t0
1:
        .option push
        .option norelax
        la      gp, __global_pointer$
        .option pop
        lla     t0, cpu_table
        mv      tp, t0
        tail    kernel_main

.park:
        wfi
        j       .park

/*
 * Physical secondary trampoline, page-aligned in .text.secondary.
 *
 * HSM calls it with a0 = actual hartid, a1 = logical CPU ID (opaque).
 * Executes entirely before satp activation: every memory address below is
 * PC-relative (physical, since kernel VA low 32 bits equal PA) or an
 * immediate constant. gp must not be used until the high-half continuation.
 * sp is physical and stays valid because the kernel page table retains the
 * DRAM identity alias.
 */
        .section .text.secondary
        .globl secondary_entry
        .balign 4096
secondary_entry:
        /* Interrupts off: OpenSBI may return with SIE/SPIE set. */
        csrc    sstatus, SSTATUS_SIE
        csrw    sie, zero

        /* Validate the logical ID before indexing any static storage. */
        li      t0, CONFIG_QEMU_CPUS
        bltu    a1, t0, 1f
        j       .Lsec_park
1:
        /* sp = idle_stacks[logical_id] + KSTACK_SIZE (physical). */
        lla     t0, idle_stacks
        li      t1, TASK_KSTACK_SIZE
        mul     t1, a1, t1
        add     t0, t0, t1
        add     sp, t0, t1

        /* Acquire-load the published kernel satp token; reject zero/Sv39
         * mismatch and park with a boot error record. */
        lla     t0, pgtable_boot_token
        ld      t1, 0(t0)
        fence   r, r
        beqz    t1, .Lsec_bad_satp
        srli    t2, t1, 60
        li      t3, SATP_MODE_SV39_VAL
        bne     t2, t3, .Lsec_bad_satp

        csrw    satp, t1
        sfence.vma

        /* Jump to the high-half continuation (_start pattern): after the
         * jump the PC is high, so lla/la yield high-half addresses. */
        lla     t0, 2f
        li      t1, 0xFFFFFFC000000000
        add     t0, t0, t1
        jr      t0
2:
        .option push
        .option norelax
        la      gp, __global_pointer$
        .option pop
        /* tp is installed by smp_secondary_main() as its first statement;
         * tp is a reserved register, never allocated by the compiler. */
        tail    smp_secondary_main

.Lsec_bad_satp:
        lla     t0, smp_boot_errors
        slli    t1, a1, 2
        add     t0, t0, t1
        li      t1, SMP_BOOT_ERR_NO_SATP
        sw      t1, 0(t0)
.Lsec_park:
        wfi
        j       .Lsec_park


        .section .bss
        .align  12
tmp_root:
        .space  4096
