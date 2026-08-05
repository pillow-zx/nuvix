/*
 * arch/riscv/timer.c - Sstc 时钟 (100Hz)
 */

#include <kernel/timer.h>
#include <asm/csr.h>

uint64_t timer_now(void)
{
	return csr_read(time);
}

void timer_set(uint64_t value)
{
	csr_write(stimecmp, value);
}

void timer_init(void)
{
	timer_set(timer_now() + CLOCKS_PER_TICK);
}
