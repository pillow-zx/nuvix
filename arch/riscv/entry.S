#include <asm/csr.h>
#include <asm/trap.h>

    .section .text, "ax"
    .balign 4
    .globl trap_entry
    .type trap_entry, @function
trap_entry:
    /* Fatal-only entry: use sscratch to preserve t0 before any stack access. */
    csrrw       t0, sscratch, t0
    lla         t0, .Ltrap_halt
    csrw        stvec, t0
    csrrw       t0, sscratch, t0

    addi        sp, sp, -TRAP_SIZE
    sd          zero, TRAP_REG(0)(sp)
    sd          ra, TRAP_REG(1)(sp)
    sd          gp, TRAP_REG(3)(sp)
    sd          tp, TRAP_REG(4)(sp)
    sd          t0, TRAP_REG(5)(sp)
    sd          t1, TRAP_REG(6)(sp)
    sd          t2, TRAP_REG(7)(sp)
    sd          s0, TRAP_REG(8)(sp)
    sd          s1, TRAP_REG(9)(sp)
    sd          a0, TRAP_REG(10)(sp)
    sd          a1, TRAP_REG(11)(sp)
    sd          a2, TRAP_REG(12)(sp)
    sd          a3, TRAP_REG(13)(sp)
    sd          a4, TRAP_REG(14)(sp)
    sd          a5, TRAP_REG(15)(sp)
    sd          a6, TRAP_REG(16)(sp)
    sd          a7, TRAP_REG(17)(sp)
    sd          s2, TRAP_REG(18)(sp)
    sd          s3, TRAP_REG(19)(sp)
    sd          s4, TRAP_REG(20)(sp)
    sd          s5, TRAP_REG(21)(sp)
    sd          s6, TRAP_REG(22)(sp)
    sd          s7, TRAP_REG(23)(sp)
    sd          s8, TRAP_REG(24)(sp)
    sd          s9, TRAP_REG(25)(sp)
    sd          s10, TRAP_REG(26)(sp)
    sd          s11, TRAP_REG(27)(sp)
    sd          t3, TRAP_REG(28)(sp)
    sd          t4, TRAP_REG(29)(sp)
    sd          t5, TRAP_REG(30)(sp)
    sd          t6, TRAP_REG(31)(sp)

    addi        t0, sp, TRAP_SIZE
    sd          t0, TRAP_REG(2)(sp)
    csrr        t0, sepc
    sd          t0, TRAP_SEPC(sp)
    csrr        t0, sstatus
    sd          t0, TRAP_SSTATUS(sp)
    csrr        t0, scause
    sd          t0, TRAP_SCAUSE(sp)
    csrr        t0, stval
    sd          t0, TRAP_STVAL(sp)
    csrr        t0, satp
    sd          t0, TRAP_SATP(sp)

#ifdef CONFIG_FPU
    /* FS=Off prohibits both FP register access and reading fcsr. */
    ld          t0, TRAP_SSTATUS(sp)
    li          t1, SSTATUS_FS
    and         t0, t0, t1
    beqz        t0, .Lno_fpu

    fsd         f0, TRAP_FREG(0)(sp)
    fsd         f1, TRAP_FREG(1)(sp)
    fsd         f2, TRAP_FREG(2)(sp)
    fsd         f3, TRAP_FREG(3)(sp)
    fsd         f4, TRAP_FREG(4)(sp)
    fsd         f5, TRAP_FREG(5)(sp)
    fsd         f6, TRAP_FREG(6)(sp)
    fsd         f7, TRAP_FREG(7)(sp)
    fsd         f8, TRAP_FREG(8)(sp)
    fsd         f9, TRAP_FREG(9)(sp)
    fsd         f10, TRAP_FREG(10)(sp)
    fsd         f11, TRAP_FREG(11)(sp)
    fsd         f12, TRAP_FREG(12)(sp)
    fsd         f13, TRAP_FREG(13)(sp)
    fsd         f14, TRAP_FREG(14)(sp)
    fsd         f15, TRAP_FREG(15)(sp)
    fsd         f16, TRAP_FREG(16)(sp)
    fsd         f17, TRAP_FREG(17)(sp)
    fsd         f18, TRAP_FREG(18)(sp)
    fsd         f19, TRAP_FREG(19)(sp)
    fsd         f20, TRAP_FREG(20)(sp)
    fsd         f21, TRAP_FREG(21)(sp)
    fsd         f22, TRAP_FREG(22)(sp)
    fsd         f23, TRAP_FREG(23)(sp)
    fsd         f24, TRAP_FREG(24)(sp)
    fsd         f25, TRAP_FREG(25)(sp)
    fsd         f26, TRAP_FREG(26)(sp)
    fsd         f27, TRAP_FREG(27)(sp)
    fsd         f28, TRAP_FREG(28)(sp)
    fsd         f29, TRAP_FREG(29)(sp)
    fsd         f30, TRAP_FREG(30)(sp)
    fsd         f31, TRAP_FREG(31)(sp)
    csrr        t0, fcsr
    sd          t0, TRAP_FCSR(sp)
    j           .Lfpu_saved

.Lno_fpu:
    /* No active FP context: make the unused frame fields deterministic. */
    addi        t0, sp, TRAP_FREG(0)
    addi        t1, sp, TRAP_SIZE
.Lclear_fpu:
    sd          zero, 0(t0)
    addi        t0, t0, TRAP_REG_SIZE
    bltu        t0, t1, .Lclear_fpu
.Lfpu_saved:
#endif

    mv          a0, sp
    call        trap_handler
    j           .Ltrap_halt
    .size trap_entry, . - trap_entry

    .balign 4
    /* Recursive traps can have a broken stack: keep this exit stackless. */
.Ltrap_halt:
    csrci       sstatus, SSTATUS_SIE
.Lhalt:
    wfi
    j           .Lhalt
