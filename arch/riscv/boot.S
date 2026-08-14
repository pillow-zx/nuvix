#include <asm/asm_offsets.h>
#include <kernel/smp.h>

#define SSTATUS_SIE (1 << 1)
#define SATP_MODE_SV39_VAL 8

#define PTE_V   (1 << 0)
#define PTE_R   (1 << 1)
#define PTE_W   (1 << 2)
#define PTE_X   (1 << 3)
#define PTE_G   (1 << 5)
#define PTE_U   (1 << 4)

#define PTE_MEGA_DRAM_FLAGS  (PTE_V | PTE_R | PTE_W | PTE_X | PTE_G)
#define PTE_MEGA_MMIO_FLAGS  (PTE_V | PTE_R | PTE_W | PTE_G)

	.section .text.entry
	.globl _start
_start:
	lla	gp, __global_pointer$

	lla	sp, idle_stacks + TASK_KSTACK_SIZE

	lla	t0, __bss_start
	lla	t1, _end
1:	sd	zero, 0(t0)
	addi	t0, t0, 8
	bne	t0, t1, 1b

	lla	t0, tmp_root

	li	t1, (0x80000000 >> 2) | PTE_MEGA_DRAM_FLAGS
	sd	t1, (2*8)(t0)

	li	t2, 258*8
	add	t2, t0, t2
	sd	t1, 0(t2)

	li	t1, (0x00000000 >> 2) | PTE_MEGA_MMIO_FLAGS
	sd	t1, (0*8)(t0)

	lla	t0, tmp_root
	srli	t0, t0, 12
	li	t1, (8 << 60)
	or	t0, t0, t1
	csrw	satp, t0
	sfence.vma

	lla	t0, 1f
	li	t1, 0xFFFFFFC000000000
	add	t0, t0, t1
	jr	t0
1:	lla	gp, __global_pointer$
	lla	t0, cpu_table
	mv	tp, t0
	tail	kernel_main

	.section .text.secondary
	.globl secondary_entry
	.balign 4096
secondary_entry:
	csrc	sstatus, SSTATUS_SIE
	csrw	sie, zero

	li	t0, CONFIG_QEMU_CPUS
	bltu	a1, t0, 1f
	j	.Lsec_park
1:	lla	t0, idle_stacks
	li	t1, TASK_KSTACK_SIZE
	mul	t2, a1, t1
	add	sp, t0, t2
	add	sp, sp, t1

	lla	t0, pgtable_boot_token
	ld	t1, 0(t0)
	fence	r, r
	beqz	t1, .Lsec_bad_satp
	srli	t2, t1, 60
	li	t3, SATP_MODE_SV39_VAL
	bne	t2, t3, .Lsec_bad_satp

	csrw	satp, t1
	sfence.vma

	lla	t0, 2f
	li	t1, 0xFFFFFFC000000000
	add	t0, t0, t1
	jr	t0
2:	lla	gp, __global_pointer$
	tail	smp_secondary_main

.Lsec_bad_satp:
	lla	t0, smp_boot_errors
	slli	t1, a1, 2
	add	t0, t0, t1
	li	t1, SMP_BOOT_ERR_NO_SATP
	sw	t1, 0(t0)

	lla	t0, cpu_table
	li	t1, CPU_SIZE
	mul	t1, a1, t1
	add	t0, t0, t1
	li	t1, 3
	fence	w, w
	sw	t1, CPU_STATE(t0)
.Lsec_park:
	wfi
	j	.Lsec_park

	.section .bss
	.align	12
tmp_root:
	.space	4096
