#include <asm/csr.h>
#include <asm/task.h>

    .section .text, "ax"
    .balign 4
    .globl __switch_to
    .type __switch_to, @function
__switch_to:
    /* Preserve a0: the resumed continuation receives the outgoing task. */
    sd          ra, TASK_CTX_RA(a0)
    sd          sp, TASK_CTX_SP(a0)
    sd          s0, TASK_CTX_S0(a0)
    sd          s1, TASK_CTX_S1(a0)
    sd          s2, TASK_CTX_S2(a0)
    sd          s3, TASK_CTX_S3(a0)
    sd          s4, TASK_CTX_S4(a0)
    sd          s5, TASK_CTX_S5(a0)
    sd          s6, TASK_CTX_S6(a0)
    sd          s7, TASK_CTX_S7(a0)
    sd          s8, TASK_CTX_S8(a0)
    sd          s9, TASK_CTX_S9(a0)
    sd          s10, TASK_CTX_S10(a0)
    sd          s11, TASK_CTX_S11(a0)

    li          t1, SSTATUS_SUM
    csrrc       t0, sstatus, t1
    and         t0, t0, t1
    sd          t0, TASK_CTX_SUM(a0)

#ifdef CONFIG_FPU
    csrr        t0, sstatus
    li          t1, SSTATUS_FS
    and         t0, t0, t1
    sd          t0, TASK_CTX_FS(a0)
    beqz        t0, .Lrestore_fpu

    fsd         f0, TASK_CTX_FREG(0)(a0)
    fsd         f1, TASK_CTX_FREG(1)(a0)
    fsd         f2, TASK_CTX_FREG(2)(a0)
    fsd         f3, TASK_CTX_FREG(3)(a0)
    fsd         f4, TASK_CTX_FREG(4)(a0)
    fsd         f5, TASK_CTX_FREG(5)(a0)
    fsd         f6, TASK_CTX_FREG(6)(a0)
    fsd         f7, TASK_CTX_FREG(7)(a0)
    fsd         f8, TASK_CTX_FREG(8)(a0)
    fsd         f9, TASK_CTX_FREG(9)(a0)
    fsd         f10, TASK_CTX_FREG(10)(a0)
    fsd         f11, TASK_CTX_FREG(11)(a0)
    fsd         f12, TASK_CTX_FREG(12)(a0)
    fsd         f13, TASK_CTX_FREG(13)(a0)
    fsd         f14, TASK_CTX_FREG(14)(a0)
    fsd         f15, TASK_CTX_FREG(15)(a0)
    fsd         f16, TASK_CTX_FREG(16)(a0)
    fsd         f17, TASK_CTX_FREG(17)(a0)
    fsd         f18, TASK_CTX_FREG(18)(a0)
    fsd         f19, TASK_CTX_FREG(19)(a0)
    fsd         f20, TASK_CTX_FREG(20)(a0)
    fsd         f21, TASK_CTX_FREG(21)(a0)
    fsd         f22, TASK_CTX_FREG(22)(a0)
    fsd         f23, TASK_CTX_FREG(23)(a0)
    fsd         f24, TASK_CTX_FREG(24)(a0)
    fsd         f25, TASK_CTX_FREG(25)(a0)
    fsd         f26, TASK_CTX_FREG(26)(a0)
    fsd         f27, TASK_CTX_FREG(27)(a0)
    fsd         f28, TASK_CTX_FREG(28)(a0)
    fsd         f29, TASK_CTX_FREG(29)(a0)
    fsd         f30, TASK_CTX_FREG(30)(a0)
    fsd         f31, TASK_CTX_FREG(31)(a0)
    csrr        t0, fcsr
    sd          t0, TASK_CTX_FCSR(a0)

.Lrestore_fpu:
    ld          t0, TASK_CTX_FS(a1)
    li          t1, SSTATUS_FS
    csrc        sstatus, t1
    beqz        t0, .Lfpu_done
    csrs        sstatus, t0

    fld         f0, TASK_CTX_FREG(0)(a1)
    fld         f1, TASK_CTX_FREG(1)(a1)
    fld         f2, TASK_CTX_FREG(2)(a1)
    fld         f3, TASK_CTX_FREG(3)(a1)
    fld         f4, TASK_CTX_FREG(4)(a1)
    fld         f5, TASK_CTX_FREG(5)(a1)
    fld         f6, TASK_CTX_FREG(6)(a1)
    fld         f7, TASK_CTX_FREG(7)(a1)
    fld         f8, TASK_CTX_FREG(8)(a1)
    fld         f9, TASK_CTX_FREG(9)(a1)
    fld         f10, TASK_CTX_FREG(10)(a1)
    fld         f11, TASK_CTX_FREG(11)(a1)
    fld         f12, TASK_CTX_FREG(12)(a1)
    fld         f13, TASK_CTX_FREG(13)(a1)
    fld         f14, TASK_CTX_FREG(14)(a1)
    fld         f15, TASK_CTX_FREG(15)(a1)
    fld         f16, TASK_CTX_FREG(16)(a1)
    fld         f17, TASK_CTX_FREG(17)(a1)
    fld         f18, TASK_CTX_FREG(18)(a1)
    fld         f19, TASK_CTX_FREG(19)(a1)
    fld         f20, TASK_CTX_FREG(20)(a1)
    fld         f21, TASK_CTX_FREG(21)(a1)
    fld         f22, TASK_CTX_FREG(22)(a1)
    fld         f23, TASK_CTX_FREG(23)(a1)
    fld         f24, TASK_CTX_FREG(24)(a1)
    fld         f25, TASK_CTX_FREG(25)(a1)
    fld         f26, TASK_CTX_FREG(26)(a1)
    fld         f27, TASK_CTX_FREG(27)(a1)
    fld         f28, TASK_CTX_FREG(28)(a1)
    fld         f29, TASK_CTX_FREG(29)(a1)
    fld         f30, TASK_CTX_FREG(30)(a1)
    fld         f31, TASK_CTX_FREG(31)(a1)
    ld          t1, TASK_CTX_FCSR(a1)
    csrw        fcsr, t1
    /* Restore only FS, including its saved clean/dirty state. */
    li          t1, SSTATUS_FS
    csrc        sstatus, t1
    csrs        sstatus, t0
.Lfpu_done:
#endif

    ld          ra, TASK_CTX_RA(a1)
    ld          sp, TASK_CTX_SP(a1)
    ld          s0, TASK_CTX_S0(a1)
    ld          s1, TASK_CTX_S1(a1)
    ld          s2, TASK_CTX_S2(a1)
    ld          s3, TASK_CTX_S3(a1)
    ld          s4, TASK_CTX_S4(a1)
    ld          s5, TASK_CTX_S5(a1)
    ld          s6, TASK_CTX_S6(a1)
    ld          s7, TASK_CTX_S7(a1)
    ld          s8, TASK_CTX_S8(a1)
    ld          s9, TASK_CTX_S9(a1)
    ld          s10, TASK_CTX_S10(a1)
    ld          s11, TASK_CTX_S11(a1)
    ld          t0, TASK_CTX_SUM(a1)
    li          t1, SSTATUS_SUM
    csrc        sstatus, t1
    csrs        sstatus, t0
    mv          tp, a1
    ret
    .size __switch_to, . - __switch_to

    .balign 4
    .globl __ktask_entry
    .type __ktask_entry, @function
__ktask_entry:
    mv          a1, s0
    mv          a2, s1
    tail        ktask_entry
    .size __ktask_entry, . - __ktask_entry
