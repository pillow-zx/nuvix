#include <asm/csr.h>

    .section .text, "ax"
    .balign 4
    .globl __copy_from_user
    .globl __copy_to_user
    .type __copy_from_user, @function
    .type __copy_to_user, @function
__copy_from_user:
__copy_to_user:
    beqz        a2, .L__remaining
    li          t0, SSTATUS_SUM
    csrrs       t1, sstatus, t0
    and         t1, t1, t0
.Lcopy:
    lbu         t2, 0(a1)
    sb          t2, 0(a0)
    addi        a0, a0, 1
    addi        a1, a1, 1
    addi        a2, a2, -1
    bnez        a2, .Lcopy

    csrc        sstatus, t0
    csrs        sstatus, t1
.L__remaining:
    mv          a0, a2
    ret
    .size __copy_from_user, . - __copy_from_user
    .size __copy_to_user, . - __copy_to_user
