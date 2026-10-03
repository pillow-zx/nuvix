# riscv architecture (rv64gc, LP64).

# -mno-relax: early boot code runs with the PC below KERNEL_VBASE, so linker
# relaxation rewriting auipc+addi pairs would break the address assumptions.
ARCH_FLAGS := -march=rv64gc -mabi=lp64 -mno-relax -mcmodel=medany

ARCH_LD_EMULATION := elf64lriscv
ARCH_CC_TARGET    := riscv64-unknown-elf



obj-y += arch/riscv/boot.o
obj-y += arch/riscv/entry.S
obj-y += arch/riscv/trap.c
