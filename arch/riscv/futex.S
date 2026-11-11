#include <asm/csr.h>
#include <nuvix/errno.h>

    .section .text, "ax"
    .balign 4
    .global __futex_cmpxchg_u32
    .global __futex_cmpxchg_load32
    .global __futex_cmpxchg_store32
    .global __futex_cmpxchg_fixup
    .type __futex_cmpxchg_u32, @function
__futex_cmpxchg_u32:
    li          t0, SSTATUS_SUM
    csrrs       t1, sstatus, t0
    and         t1, t1, t0
    /* lr.w sign-extends its result on RV64. */
    sext.w      a2, a2

__futex_cmpxchg_load32:
    lr.w.aq     t2, (a1)
    bne         t2, a2, 1f
__futex_cmpxchg_store32:
    sc.w.rl     t3, a3, (a1)
    bnez        t3, __futex_cmpxchg_load32

1:
    sw          t2, 0(a0)
    li          a0, 0
    j           2f

__futex_cmpxchg_fixup:
    li          a0, -EFAULT
2:
    csrc        sstatus, t0
    csrs        sstatus, t1
    ret
    .size __futex_cmpxchg_u32, . - __futex_cmpxchg_u32
