# riscv architecture (rv64gc, LP64).

# -mno-relax: early boot code runs with the PC below KERNEL_VBASE, so linker
# relaxation rewriting auipc+addi pairs would break the address assumptions.
ARCH_FLAGS := -march=rv64gc -mabi=lp64 -mno-relax -mcmodel=medany

ARCH_LD_EMULATION := elf64lriscv
ARCH_CC_TARGET    := riscv64-unknown-elf

obj-y += arch/riscv/boot.o
obj-y += arch/riscv/entry.o
obj-y += arch/riscv/uaccess_fixup.o
obj-y += arch/riscv/switch.o
obj-y += arch/riscv/fpu.o
obj-y += arch/riscv/trap.o
obj-y += arch/riscv/task.o
obj-y += arch/riscv/timer.o
obj-y += arch/riscv/sbi.o
obj-y += arch/riscv/platform.o
obj-$(CONFIG_SMP) += arch/riscv/smp.o

include $(srctree)/arch/riscv/mm/mm.mk
include $(srctree)/arch/riscv/lib/lib.mk
