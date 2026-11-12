#include <nuvix/tick.h>
#include <nuvix/module.h>
#include <nuvix/compiler.h>
#include <nuvix/dtb.h>
#include <nuvix/cpu.h>
#include <nuvix/sched.h>
#include <nuvix/timer.h>
#include <nuvix/errno.h>
#include <nuvix/printk.h>

static uint32_t timebase_frequency;
static uint64_t tick_period;

/* Boot-only clock setup, before CPU startup. */
static int tick_init(void)
{
    int ret = dtb_get_u32("/cpus", "timebase-frequency", &timebase_frequency);

    if (ret)
        return ret;
    if (timebase_frequency < MSEC_PER_SEC)
        return -EINVAL;
    tick_period = timebase_frequency / MSEC_PER_SEC;
    pr_info("clock: timebase=%u Hz, tick_period=%u\n", timebase_frequency,
            (unsigned int)tick_period);
    return 0;
}
MODULE_INIT(tick_init, 40);

uint64_t time_ticks_from_ms(unsigned int ms)
{
    return (uint64_t)timebase_frequency * ms / MSEC_PER_SEC;
}

void time_timespec_from_ticks(uint64_t ticks, struct timespec64 *ts)
{
    ts->tv_sec = ticks / timebase_frequency;
    ts->tv_nsec =
        (ticks % timebase_frequency) * NSEC_PER_SEC / timebase_frequency;
}

int time_ns_from_ticks(uint64_t ticks, uint64_t *ns)
{
    struct timespec64 ts;
    uint64_t value;

    time_timespec_from_ticks(ticks, &ts);
    if (check_mul_overflow((uint64_t)ts.tv_sec, NSEC_PER_SEC, &value) ||
        check_add_overflow(value, (uint64_t)ts.tv_nsec, &value))
        return -EOVERFLOW;
    *ns = value;
    return 0;
}

int time_ticks_from_timespec(const struct timespec64 *ts, uint64_t *ticks)
{
    uint64_t value, fraction;

    if (ts->tv_sec < 0 || ts->tv_nsec < 0 ||
        ts->tv_nsec >= (int64_t)NSEC_PER_SEC)
        return -EINVAL;
    if (check_mul_overflow((uint64_t)ts->tv_sec, timebase_frequency, &value))
        return -EOVERFLOW;
    fraction = (uint64_t)ts->tv_nsec * timebase_frequency;
    fraction = fraction / NSEC_PER_SEC + (fraction % NSEC_PER_SEC != 0);
    if (check_add_overflow(value, fraction, &value))
        return -EOVERFLOW;
    *ticks = value;
    return 0;
}

int time_ticks_from_ns(uint64_t ns, uint64_t *ticks)
{
    struct timespec64 ts = {
        .tv_sec = ns / NSEC_PER_SEC,
        .tv_nsec = ns % NSEC_PER_SEC,
    };

    return time_ticks_from_timespec(&ts, ticks);
}

void time_get_monotonic(struct timespec64 *ts)
{
    time_timespec_from_ticks(time_read(), ts);
}

int time_deadline_from_ns(uint64_t ns, uint64_t *deadline)
{
    uint64_t now = time_read();
    uint64_t ticks, value;
    int ret = time_ticks_from_ns(ns, &ticks);

    if (ret)
        return ret;
    if (check_add_overflow(now, ticks, &value))
        return -EOVERFLOW;
    *deadline = value;
    return 0;
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
