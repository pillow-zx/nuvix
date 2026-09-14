
#include <asm/asm_offsets.h>

#define SSTATUS_SIE  (1UL << 1)

	.section .text.entry
	.globl __alltraps
	.globl __trapret
	.globl trapret_to_user
	.align 4

__alltraps:
	csrrw   tp, sscratch, tp
	bnez    tp, .Lfrom_user

	addi    sp, sp, -TRAP_FRAME_ALLOC_SIZE

	sd      t1, TF_T1(sp)
	csrr    t1, sscratch
	csrw    sscratch, zero
	mv      tp, t1
	addi    t1, sp, TRAP_FRAME_ALLOC_SIZE
	sd      t1, TF_SP(sp)

	sd      ra,  TF_RA(sp)
	sd      gp,  TF_GP(sp)
	sd      tp,  TF_TP(sp)
	sd      t0,  TF_T0(sp)
	sd      t2,  TF_T2(sp)
	sd      s0,  TF_S0(sp)
	sd      s1,  TF_S1(sp)
	sd      a0,  TF_A0(sp)
	sd      a1,  TF_A1(sp)
	sd      a2,  TF_A2(sp)
	sd      a3,  TF_A3(sp)
	sd      a4,  TF_A4(sp)
	sd      a5,  TF_A5(sp)
	sd      a6,  TF_A6(sp)
	sd      a7,  TF_A7(sp)
	sd      s2,  TF_S2(sp)
	sd      s3,  TF_S3(sp)
	sd      s4,  TF_S4(sp)
	sd      s5,  TF_S5(sp)
	sd      s6,  TF_S6(sp)
	sd      s7,  TF_S7(sp)
	sd      s8,  TF_S8(sp)
	sd      s9,  TF_S9(sp)
	sd      s10, TF_S10(sp)
	sd      s11, TF_S11(sp)
	sd      t3,  TF_T3(sp)
	sd      t4,  TF_T4(sp)
	sd      t5,  TF_T5(sp)
	sd      t6,  TF_T6(sp)

	csrr    t1, sepc
	sd      t1, TF_SEPC(sp)
	csrr    t1, scause
	sd      t1, TF_SCAUSE(sp)
	csrr    t1, stval
	sd      t1, TF_STVAL(sp)
	csrr    t1, sstatus
	sd      t1, TF_SSTATUS(sp)

	j       .Lcall_handler

.Lfrom_user:
	sd      t0, CPU_ENTRY_SCRATCH(tp)
	sd      t1, CPU_ENTRY_SCRATCH + 8(tp)

	ld      t0, CPU_CURRENT_TASK(tp)
	ld      t0, TASK_KSTACK(t0)
	li      t1, TASK_KSTACK_SIZE
	add     t0, t0, t1

	sd      sp, -8(t0)

	mv      sp, t0
	addi    sp, sp, -TRAP_FRAME_ALLOC_SIZE

	ld      t0, TRAP_SCRATCH_OFFSET(sp)
	sd      t0, TF_SP(sp)

	csrr    t0, sscratch
	sd      t0, TF_TP(sp)
	csrw    sscratch, zero

	ld      t0, CPU_ENTRY_SCRATCH(tp)
	sd      t0, TF_T0(sp)
	ld      t1, CPU_ENTRY_SCRATCH + 8(tp)
	sd      t1, TF_T1(sp)

	sd      ra,  TF_RA(sp)
	sd      gp,  TF_GP(sp)
	sd      t2,  TF_T2(sp)
	sd      s0,  TF_S0(sp)
	sd      s1,  TF_S1(sp)
	sd      a0,  TF_A0(sp)
	sd      a1,  TF_A1(sp)
	sd      a2,  TF_A2(sp)
	sd      a3,  TF_A3(sp)
	sd      a4,  TF_A4(sp)
	sd      a5,  TF_A5(sp)
	sd      a6,  TF_A6(sp)
	sd      a7,  TF_A7(sp)
	sd      s2,  TF_S2(sp)
	sd      s3,  TF_S3(sp)
	sd      s4,  TF_S4(sp)
	sd      s5,  TF_S5(sp)
	sd      s6,  TF_S6(sp)
	sd      s7,  TF_S7(sp)
	sd      s8,  TF_S8(sp)
	sd      s9,  TF_S9(sp)
	sd      s10, TF_S10(sp)
	sd      s11, TF_S11(sp)
	sd      t3,  TF_T3(sp)
	sd      t4,  TF_T4(sp)
	sd      t5,  TF_T5(sp)
	sd      t6,  TF_T6(sp)

	csrr    t0, sepc
	sd      t0, TF_SEPC(sp)
	csrr    t0, scause
	sd      t0, TF_SCAUSE(sp)
	csrr    t0, stval
	sd      t0, TF_STVAL(sp)
	csrr    t0, sstatus
	sd      t0, TF_SSTATUS(sp)

.Lcall_handler:
	mv      a0, sp
	jal     ra, trap_handler

	.align 4
__trapret:
	csrci   sstatus, SSTATUS_SIE
	ld      t0, TF_SEPC(sp)
	csrw    sepc, t0
	ld      t0, TF_SCAUSE(sp)
	csrw    scause, t0
	ld      t0, TF_STVAL(sp)
	csrw    stval, t0
	ld      t0, TF_SSTATUS(sp)
	csrw    sstatus, t0

	ld      ra,  TF_RA(sp)
	ld      gp,  TF_GP(sp)
	ld      t2,  TF_T2(sp)
	ld      s0,  TF_S0(sp)
	ld      s1,  TF_S1(sp)
	ld      a0,  TF_A0(sp)
	ld      a1,  TF_A1(sp)
	ld      a2,  TF_A2(sp)
	ld      a3,  TF_A3(sp)
	ld      a4,  TF_A4(sp)
	ld      a5,  TF_A5(sp)
	ld      a6,  TF_A6(sp)
	ld      a7,  TF_A7(sp)
	ld      s2,  TF_S2(sp)
	ld      s3,  TF_S3(sp)
	ld      s4,  TF_S4(sp)
	ld      s5,  TF_S5(sp)
	ld      s6,  TF_S6(sp)
	ld      s7,  TF_S7(sp)
	ld      s8,  TF_S8(sp)
	ld      s9,  TF_S9(sp)
	ld      s10, TF_S10(sp)
	ld      s11, TF_S11(sp)
	ld      t3,  TF_T3(sp)
	ld      t4,  TF_T4(sp)
	ld      t5,  TF_T5(sp)
	ld      t6,  TF_T6(sp)

	csrr    t0, sstatus
	andi    t0, t0, (1 << 8)
	bnez    t0, .Lret_supervisor

.Lret_user:
	csrw    sscratch, tp

	ld      tp, TF_TP(sp)
	ld      t1, TF_T1(sp)
	ld      t0, TF_T0(sp)
	ld      sp, TF_SP(sp)
	sret

.Lret_supervisor:
	csrw    sscratch, zero
	ld      t1, TF_T1(sp)
	ld      t0, TF_T0(sp)
	addi    sp, sp, TRAP_FRAME_ALLOC_SIZE
	sret

trapret_to_user:
	mv      sp, a0
	j       __trapret
