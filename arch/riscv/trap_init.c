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
	/* Clear any pending SSIP before enabling SSIE: on QEMU virt the SSIP
	 * bit is set by writing 1 (not W1C), so csr_set would assert it. */
	csr_clear(sip, SIP_SSIP);
	csr_set(sie, SIE_SSIE);
}

/* CPU 0-only diagnostic; secondaries must never print. */
void trap_cpu_init_print(void)
{
	pr_info("stvec: 0x%lx, sscratch: 0x%lx, sie: 0x%lx, sstatus: 0x%lx\n",
		csr_read(stvec), csr_read(sscratch), csr_read(sie),
		csr_read(sstatus));
}
