# riscv architecture (rv64gc, LP64).

# -mno-relax: early boot code runs with the PC below KERNEL_VBASE, so linker
# relaxation rewriting auipc+addi pairs would break the address assumptions.
ARCH_FLAGS := -march=rv64gc -mabi=lp64 -mno-relax -mcmodel=medany

ARCH_LD_EMULATION := elf64lriscv
ARCH_CC_TARGET    := riscv64-unknown-elf



obj-y += arch/riscv/boot.S
obj-y += arch/riscv/entry.S
obj-y += arch/riscv/switch.S
obj-y += arch/riscv/uaccess.S
obj-y += arch/riscv/sigret.S
obj-y += arch/riscv/trap.c
obj-y += arch/riscv/task.c
obj-y += arch/riscv/irq.c
obj-y += arch/riscv/pgtable.c
obj-y += arch/riscv/cpu.c
obj-y += arch/riscv/signal.c
obj-y += arch/riscv/sys.c

obj-$(CONFIG_SMP) += arch/riscv/smp.c
obj-$(CONFIG_SOC_QEMU_VIRT) += arch/riscv/plic.c
