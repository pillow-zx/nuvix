#include <asm/csr.h>

    .section .text, "ax"
    .balign 4
    .globl __copy_from_user
    .globl __copy_to_user
    .globl __uaccess_load8
    .globl __uaccess_store8
    .globl __uaccess_load16
    .globl __uaccess_store16
    .globl __uaccess_load32
    .globl __uaccess_store32
    .globl __uaccess_load64
    .globl __uaccess_store64
    .globl __uaccess_fixup
    .type __copy_from_user, @function
    .type __copy_to_user, @function
__copy_from_user:
__copy_to_user:
    beqz        a2, .L__remaining
    li          t0, SSTATUS_SUM
    csrrs       t1, sstatus, t0
    and         t1, t1, t0

.Lcopy:
    or          t3, a0, a1
    li          t4, 8
    bltu        a2, t4, .Ltry32
    andi        t5, t3, 7
    beqz        t5, __uaccess_load64
.Ltry32:
    li          t4, 4
    bltu        a2, t4, .Ltry16
    andi        t5, t3, 3
    beqz        t5, __uaccess_load32
.Ltry16:
    li          t4, 2
    bltu        a2, t4, .Lcopy8
    andi        t5, t3, 1
    beqz        t5, __uaccess_load16

.Lcopy8:
    li          t4, 1
__uaccess_load8:
    lbu         t2, 0(a1)
__uaccess_store8:
    sb          t2, 0(a0)
    j           .Ladvance

__uaccess_load16:
    lhu         t2, 0(a1)
__uaccess_store16:
    sh          t2, 0(a0)
    j           .Ladvance

__uaccess_load32:
    lw          t2, 0(a1)
__uaccess_store32:
    sw          t2, 0(a0)
    j           .Ladvance

__uaccess_load64:
    ld          t2, 0(a1)
__uaccess_store64:
    sd          t2, 0(a0)

.Ladvance:
    add         a0, a0, t4
    add         a1, a1, t4
    sub         a2, a2, t4
    bnez        a2, .Lcopy

__uaccess_fixup:
    csrc        sstatus, t0
    csrs        sstatus, t1
.L__remaining:
    mv          a0, a2
    ret
    .size __copy_from_user, . - __copy_from_user
    .size __copy_to_user, . - __copy_to_user
