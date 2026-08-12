/*
 * arch/riscv/boot.S - 内核入口 _start
 */

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


        .section .bss
        .align  12
tmp_root:
        .space  4096
