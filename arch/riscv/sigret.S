#include <nuvix/syscall.h>

    .section .rodata.trampoline, "a"
    .balign 4
    .option norvc
    .global __sigret_start
    .type __sigret_start, @function
__sigret_start:
    li      a7, __NR_rt_sigreturn
    ecall
    .size __sigret_start, . -__sigret_start
    .global __sigret_end
__sigret_end:
