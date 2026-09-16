# Generic secondary CPU startup and inter-processor coordination.

obj-$(CONFIG_SMP) += kernel/smp/boot.o
obj-$(CONFIG_SMP) += kernel/smp/ipi.o
