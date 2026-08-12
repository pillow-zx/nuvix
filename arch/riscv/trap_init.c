/*
 * arch/riscv/trap_init.c - Trap 基础设施初始化
 */

#include <asm/csr.h>
#include <asm/trap.h>
#include <arch/trap.h>
#include <kernel/printk.h>

extern void __alltraps(void);

void trap_cpu_init(void)
{
	csr_write(stvec, __alltraps);
	csr_write(sscratch, 0);
	csr_set(sie, SIE_STIE);
	pr_info("stvec: 0x%lx, sscratch: 0x%lx, sie: 0x%lx, sstatus: 0x%lx\n",
		csr_read(stvec), csr_read(sscratch), csr_read(sie),
		csr_read(sstatus));
}
