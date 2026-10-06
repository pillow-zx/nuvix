#include <nuvix/tick.h>
#include <nuvix/dtb.h>
#include <nuvix/cpu.h>
#include <nuvix/sched.h>
#include <nuvix/timer.h>
#include <nuvix/errno.h>
#include <nuvix/printk.h>

static uint32_t timebase_frequency;
static uint64_t tick_period;

int tick_init(const struct dtb *dtb)
{
    int ret =
        dtb_get_u32(dtb, "/cpus", "timebase-frequency", &timebase_frequency);

    if (ret)
        return ret;
    if (timebase_frequency < 1000)
        return -EINVAL;
    tick_period = timebase_frequency / 1000;
    pr_info("clock: timebase=%u Hz, tick_period=%u\n", timebase_frequency,
            (unsigned int)tick_period);
    return 0;
}

uint64_t time_ticks_from_ms(unsigned int ms)
{
    return (uint64_t)timebase_frequency * ms / 1000;
}

long tick_cpu_init(void)
{
    struct cpu *cpu = &cpus[current_cpu_id()];

    cpu->next_tick = time_read() + tick_period;
    return arch_set_timer(cpu->next_tick);
}

void tick_handle(void)
{
    struct cpu *cpu = &cpus[current_cpu_id()];
    uint64_t now = time_read();

    /* Advance the absolute plan past missed ticks, without replaying them. */
    if (now >= cpu->next_tick)
        cpu->next_tick +=
            ((now - cpu->next_tick) / tick_period + 1) * tick_period;
    (void)arch_set_timer(cpu->next_tick);
    timer_tick(now);
    sched_tick(now);
}
