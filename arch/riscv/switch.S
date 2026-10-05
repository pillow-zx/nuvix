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
    mv          tp, a1
    ret
    .size __switch_to, . - __switch_to
