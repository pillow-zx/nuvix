#include <asm/riscv/csr.h>
#include <generated/offsets.h>

#define TRAP_FRAME_GPR_OFFSET(n) (TRAP_FRAME_REGS_OFFSET + (n) * TRAP_FRAME_REGS_STRIDE)
#define TRAP_FRAME_FPR_OFFSET(n) (TRAP_FRAME_FREGS_OFFSET + (n) * TRAP_FRAME_FREGS_STRIDE)

    .section .text, "ax"
    .balign 4
    .globl __alltraps
    .type __alltraps, @function
__alltraps:
    // switch t0 = old sscratch, sscratch = old t0
    csrrw       t0, sscratch, t0

    // if sscratch is not equal to 0 -> from user
    bnez        t0, .Lfrom_user

    // ktrap path:
    // t0 = 0
    // sscratch = old t0
    lla         t0, .Ltrap_halt
    csrw        stvec, t0
    csrrw       t0, sscratch, zero

    addi        sp, sp, -TRAP_FRAME_SIZE
    sd          tp, TRAP_FRAME_GPR_OFFSET(4)(sp)
    sd          t0, TRAP_FRAME_GPR_OFFSET(5)(sp)
    addi        t0, sp, TRAP_FRAME_SIZE
    sd          t0, TRAP_FRAME_GPR_OFFSET(2)(sp)
    j           .L__context

    // utrap path:
    // t0 = frame addr
    // sscratch = user t0
.Lfrom_user:
    csrw        stvec, t0
    lla         t0, .Ltrap_halt
    csrrw       t0, stvec, t0
    sd          sp, TRAP_FRAME_GPR_OFFSET(2)(t0)
    sd          tp, TRAP_FRAME_GPR_OFFSET(4)(t0)
    mv          sp, t0
    csrrw       t0, sscratch, zero
    sd          t0, TRAP_FRAME_GPR_OFFSET(5)(sp)
    ld          tp, TRAP_FRAME_TASK_OFFSET(sp)

.L__context:
    sd          tp, TRAP_FRAME_TASK_OFFSET(sp)
    sd          zero, TRAP_FRAME_GPR_OFFSET(0)(sp)
    sd          ra, TRAP_FRAME_GPR_OFFSET(1)(sp)
    sd          gp, TRAP_FRAME_GPR_OFFSET(3)(sp)
    sd          t1, TRAP_FRAME_GPR_OFFSET(6)(sp)
    sd          t2, TRAP_FRAME_GPR_OFFSET(7)(sp)
    sd          s0, TRAP_FRAME_GPR_OFFSET(8)(sp)
    sd          s1, TRAP_FRAME_GPR_OFFSET(9)(sp)
    sd          a0, TRAP_FRAME_GPR_OFFSET(10)(sp)
    sd          a1, TRAP_FRAME_GPR_OFFSET(11)(sp)
    sd          a2, TRAP_FRAME_GPR_OFFSET(12)(sp)
    sd          a3, TRAP_FRAME_GPR_OFFSET(13)(sp)
    sd          a4, TRAP_FRAME_GPR_OFFSET(14)(sp)
    sd          a5, TRAP_FRAME_GPR_OFFSET(15)(sp)
    sd          a6, TRAP_FRAME_GPR_OFFSET(16)(sp)
    sd          a7, TRAP_FRAME_GPR_OFFSET(17)(sp)
    sd          s2, TRAP_FRAME_GPR_OFFSET(18)(sp)
    sd          s3, TRAP_FRAME_GPR_OFFSET(19)(sp)
    sd          s4, TRAP_FRAME_GPR_OFFSET(20)(sp)
    sd          s5, TRAP_FRAME_GPR_OFFSET(21)(sp)
    sd          s6, TRAP_FRAME_GPR_OFFSET(22)(sp)
    sd          s7, TRAP_FRAME_GPR_OFFSET(23)(sp)
    sd          s8, TRAP_FRAME_GPR_OFFSET(24)(sp)
    sd          s9, TRAP_FRAME_GPR_OFFSET(25)(sp)
    sd          s10, TRAP_FRAME_GPR_OFFSET(26)(sp)
    sd          s11, TRAP_FRAME_GPR_OFFSET(27)(sp)
    sd          t3, TRAP_FRAME_GPR_OFFSET(28)(sp)
    sd          t4, TRAP_FRAME_GPR_OFFSET(29)(sp)
    sd          t5, TRAP_FRAME_GPR_OFFSET(30)(sp)
    sd          t6, TRAP_FRAME_GPR_OFFSET(31)(sp)

    csrr        t0, sepc
    sd          t0, TRAP_FRAME_SEPC_OFFSET(sp)
    li          t0, SSTATUS_SUM
    csrrc       t0, sstatus, t0
    sd          t0, TRAP_FRAME_SSTATUS_OFFSET(sp)
    csrr        t0, scause
    sd          t0, TRAP_FRAME_SCAUSE_OFFSET(sp)
    csrr        t0, stval
    sd          t0, TRAP_FRAME_STVAL_OFFSET(sp)
    csrr        t0, satp
    sd          t0, TRAP_FRAME_SATP_OFFSET(sp)

    ld          t0, TRAP_FRAME_SSTATUS_OFFSET(sp)
    li          t1, SSTATUS_FS
    and         t0, t0, t1
    beqz        t0, .Lno_fpu_save

    fsd         f0, TRAP_FRAME_FPR_OFFSET(0)(sp)
    fsd         f1, TRAP_FRAME_FPR_OFFSET(1)(sp)
    fsd         f2, TRAP_FRAME_FPR_OFFSET(2)(sp)
    fsd         f3, TRAP_FRAME_FPR_OFFSET(3)(sp)
    fsd         f4, TRAP_FRAME_FPR_OFFSET(4)(sp)
    fsd         f5, TRAP_FRAME_FPR_OFFSET(5)(sp)
    fsd         f6, TRAP_FRAME_FPR_OFFSET(6)(sp)
    fsd         f7, TRAP_FRAME_FPR_OFFSET(7)(sp)
    fsd         f8, TRAP_FRAME_FPR_OFFSET(8)(sp)
    fsd         f9, TRAP_FRAME_FPR_OFFSET(9)(sp)
    fsd         f10, TRAP_FRAME_FPR_OFFSET(10)(sp)
    fsd         f11, TRAP_FRAME_FPR_OFFSET(11)(sp)
    fsd         f12, TRAP_FRAME_FPR_OFFSET(12)(sp)
    fsd         f13, TRAP_FRAME_FPR_OFFSET(13)(sp)
    fsd         f14, TRAP_FRAME_FPR_OFFSET(14)(sp)
    fsd         f15, TRAP_FRAME_FPR_OFFSET(15)(sp)
    fsd         f16, TRAP_FRAME_FPR_OFFSET(16)(sp)
    fsd         f17, TRAP_FRAME_FPR_OFFSET(17)(sp)
    fsd         f18, TRAP_FRAME_FPR_OFFSET(18)(sp)
    fsd         f19, TRAP_FRAME_FPR_OFFSET(19)(sp)
    fsd         f20, TRAP_FRAME_FPR_OFFSET(20)(sp)
    fsd         f21, TRAP_FRAME_FPR_OFFSET(21)(sp)
    fsd         f22, TRAP_FRAME_FPR_OFFSET(22)(sp)
    fsd         f23, TRAP_FRAME_FPR_OFFSET(23)(sp)
    fsd         f24, TRAP_FRAME_FPR_OFFSET(24)(sp)
    fsd         f25, TRAP_FRAME_FPR_OFFSET(25)(sp)
    fsd         f26, TRAP_FRAME_FPR_OFFSET(26)(sp)
    fsd         f27, TRAP_FRAME_FPR_OFFSET(27)(sp)
    fsd         f28, TRAP_FRAME_FPR_OFFSET(28)(sp)
    fsd         f29, TRAP_FRAME_FPR_OFFSET(29)(sp)
    fsd         f30, TRAP_FRAME_FPR_OFFSET(30)(sp)
    fsd         f31, TRAP_FRAME_FPR_OFFSET(31)(sp)
    csrr        t0, fcsr
    sd          t0, TRAP_FRAME_FCSR_OFFSET(sp)

.Lno_fpu_save:

    lla         t0, __alltraps
    csrw        stvec, t0
    mv          a0, sp
    call        trap_handler
    j           __trapret
    .size __alltraps, . - __alltraps

    .balign 4
    .globl __trapret
    .type __trapret, @function
__trapret:
    csrci       sstatus, SSTATUS_SIE
    ld          t0, TRAP_FRAME_SEPC_OFFSET(sp)
    csrw        sepc, t0
    ld          t0, TRAP_FRAME_SSTATUS_OFFSET(sp)
    csrw        sstatus, t0

    li          t1, SSTATUS_FS
    and         t1, t0, t1
    beqz        t1, .Lno_fpu_restore

    fld         f0, TRAP_FRAME_FPR_OFFSET(0)(sp)
    fld         f1, TRAP_FRAME_FPR_OFFSET(1)(sp)
    fld         f2, TRAP_FRAME_FPR_OFFSET(2)(sp)
    fld         f3, TRAP_FRAME_FPR_OFFSET(3)(sp)
    fld         f4, TRAP_FRAME_FPR_OFFSET(4)(sp)
    fld         f5, TRAP_FRAME_FPR_OFFSET(5)(sp)
    fld         f6, TRAP_FRAME_FPR_OFFSET(6)(sp)
    fld         f7, TRAP_FRAME_FPR_OFFSET(7)(sp)
    fld         f8, TRAP_FRAME_FPR_OFFSET(8)(sp)
    fld         f9, TRAP_FRAME_FPR_OFFSET(9)(sp)
    fld         f10, TRAP_FRAME_FPR_OFFSET(10)(sp)
    fld         f11, TRAP_FRAME_FPR_OFFSET(11)(sp)
    fld         f12, TRAP_FRAME_FPR_OFFSET(12)(sp)
    fld         f13, TRAP_FRAME_FPR_OFFSET(13)(sp)
    fld         f14, TRAP_FRAME_FPR_OFFSET(14)(sp)
    fld         f15, TRAP_FRAME_FPR_OFFSET(15)(sp)
    fld         f16, TRAP_FRAME_FPR_OFFSET(16)(sp)
    fld         f17, TRAP_FRAME_FPR_OFFSET(17)(sp)
    fld         f18, TRAP_FRAME_FPR_OFFSET(18)(sp)
    fld         f19, TRAP_FRAME_FPR_OFFSET(19)(sp)
    fld         f20, TRAP_FRAME_FPR_OFFSET(20)(sp)
    fld         f21, TRAP_FRAME_FPR_OFFSET(21)(sp)
    fld         f22, TRAP_FRAME_FPR_OFFSET(22)(sp)
    fld         f23, TRAP_FRAME_FPR_OFFSET(23)(sp)
    fld         f24, TRAP_FRAME_FPR_OFFSET(24)(sp)
    fld         f25, TRAP_FRAME_FPR_OFFSET(25)(sp)
    fld         f26, TRAP_FRAME_FPR_OFFSET(26)(sp)
    fld         f27, TRAP_FRAME_FPR_OFFSET(27)(sp)
    fld         f28, TRAP_FRAME_FPR_OFFSET(28)(sp)
    fld         f29, TRAP_FRAME_FPR_OFFSET(29)(sp)
    fld         f30, TRAP_FRAME_FPR_OFFSET(30)(sp)
    fld         f31, TRAP_FRAME_FPR_OFFSET(31)(sp)
    ld          t1, TRAP_FRAME_FCSR_OFFSET(sp)
    csrw        fcsr, t1
    csrw        sstatus, t0
.Lno_fpu_restore:

    /* Clear any reservation using the zero register slot at sp. */
    sc.d        zero, zero, (sp)
    ld          ra, TRAP_FRAME_GPR_OFFSET(1)(sp)
    ld          gp, TRAP_FRAME_GPR_OFFSET(3)(sp)
    ld          t1, TRAP_FRAME_GPR_OFFSET(6)(sp)
    ld          t2, TRAP_FRAME_GPR_OFFSET(7)(sp)
    ld          s0, TRAP_FRAME_GPR_OFFSET(8)(sp)
    ld          s1, TRAP_FRAME_GPR_OFFSET(9)(sp)
    ld          a0, TRAP_FRAME_GPR_OFFSET(10)(sp)
    ld          a1, TRAP_FRAME_GPR_OFFSET(11)(sp)
    ld          a2, TRAP_FRAME_GPR_OFFSET(12)(sp)
    ld          a3, TRAP_FRAME_GPR_OFFSET(13)(sp)
    ld          a4, TRAP_FRAME_GPR_OFFSET(14)(sp)
    ld          a5, TRAP_FRAME_GPR_OFFSET(15)(sp)
    ld          a6, TRAP_FRAME_GPR_OFFSET(16)(sp)
    ld          a7, TRAP_FRAME_GPR_OFFSET(17)(sp)
    ld          s2, TRAP_FRAME_GPR_OFFSET(18)(sp)
    ld          s3, TRAP_FRAME_GPR_OFFSET(19)(sp)
    ld          s4, TRAP_FRAME_GPR_OFFSET(20)(sp)
    ld          s5, TRAP_FRAME_GPR_OFFSET(21)(sp)
    ld          s6, TRAP_FRAME_GPR_OFFSET(22)(sp)
    ld          s7, TRAP_FRAME_GPR_OFFSET(23)(sp)
    ld          s8, TRAP_FRAME_GPR_OFFSET(24)(sp)
    ld          s9, TRAP_FRAME_GPR_OFFSET(25)(sp)
    ld          s10, TRAP_FRAME_GPR_OFFSET(26)(sp)
    ld          s11, TRAP_FRAME_GPR_OFFSET(27)(sp)
    ld          t3, TRAP_FRAME_GPR_OFFSET(28)(sp)
    ld          t4, TRAP_FRAME_GPR_OFFSET(29)(sp)
    ld          t5, TRAP_FRAME_GPR_OFFSET(30)(sp)
    ld          t6, TRAP_FRAME_GPR_OFFSET(31)(sp)

    csrr        t0, sstatus
    andi        t0, t0, SSTATUS_SPP
    bnez        t0, .Lret_super

    sd          tp, TRAP_FRAME_TASK_OFFSET(sp)
    csrw        sscratch, sp
    ld          tp, TRAP_FRAME_GPR_OFFSET(4)(sp)
    ld          t0, TRAP_FRAME_GPR_OFFSET(5)(sp)
    ld          sp, TRAP_FRAME_GPR_OFFSET(2)(sp)
    sret

.Lret_super:
    csrw        sscratch, zero
    ld          tp, TRAP_FRAME_GPR_OFFSET(4)(sp)
    ld          t0, TRAP_FRAME_GPR_OFFSET(5)(sp)
    ld          sp, TRAP_FRAME_GPR_OFFSET(2)(sp)
    sret
    .size __trapret, . - __trapret

    .balign 4
.Ltrap_halt:
    csrci       sstatus, SSTATUS_SIE
.Lhalt:
    wfi
    j           .Lhalt
