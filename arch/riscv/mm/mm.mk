# riscv virtual memory.

obj-y += arch/riscv/mm/page_table.o
obj-y += arch/riscv/mm/mmio.o
obj-y += arch/riscv/mm/tlb.o
obj-$(CONFIG_SMP) += arch/riscv/mm/shootdown.o
