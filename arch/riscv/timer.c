
#include <nuvix/timer.h>
#include <asm/csr.h>
#include <arch/sbi.h>
#include <nuvix/printk.h>

uint64_t timer_frequency;
uint64_t timer_tick_interval;
static bool use_sstc;

void timer_init(uint32_t frequency, bool sstc)
{
	/* Bounds keep nanosecond conversions in uint64_t and resolution >= 1ns. */
	if (frequency < HZ || frequency > 1000000000U)
		panic("timer: unsupported timebase frequency %u", frequency);

	timer_frequency = frequency;
	timer_tick_interval = frequency / HZ;
	use_sstc = sstc;

	if (!sstc) {
		struct sbi_ret ret = sbi_probe_extension(SBI_EID_TIME);
		if (ret.error || !ret.value)
			panic("timer: neither Sstc nor SBI TIME is available");
	}
}

uint64_t timer_now(void)
{
	return csr_read(time);
}

void timer_set(uint64_t value)
{
	if (use_sstc)
		csr_write(stimecmp, value);
	else if (sbi_set_timer(value).error)
		panic("timer: SBI set_timer failed");
}

void timer_cpu_init(void)
{
	timer_set(timer_now() + timer_tick_interval);
}
