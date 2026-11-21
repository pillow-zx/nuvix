#include <nuvix/tick.h>
#include <nuvix/module.h>
#include <nuvix/compiler.h>
#include <nuvix/dtb.h>
#include <nuvix/cpu.h>
#include <nuvix/sched.h>
#include <nuvix/timer.h>
#include <nuvix/errno.h>
#include <nuvix/printk.h>
#include <nuvix/rtc.h>

static uint32_t timebase_frequency;
static uint64_t tick_period;

IFDEF(CONFIG_RTC_DEVICE, static uint64_t rt_base_ns);
IFDEF(CONFIG_RTC_DEVICE, static uint64_t rt_base_ticks);

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

    if (now >= cpu->next_tick)
        cpu->next_tick +=
            ((now - cpu->next_tick) / tick_period + 1) * tick_period;
    (void)arch_set_timer(cpu->next_tick);
    timer_tick(now);
    sched_tick(now);
}

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

#ifdef CONFIG_RTC_DEVICE
int time_get_realtime(struct timespec64 *ts)
{
    uint64_t elapsed_ns, ns;
    int ret;

    ret = time_ns_from_ticks(time_read() - rt_base_ticks, &elapsed_ns);
    if (ret)
        return ret;
    if (check_add_overflow(rt_base_ns, elapsed_ns, &ns))
        return -EOVERFLOW;
    ts->tv_sec = ns / NSEC_PER_SEC;
    ts->tv_nsec = ns % NSEC_PER_SEC;
    return 0;
}

int time_realtime_to_ticks(const struct timespec64 *ts, uint64_t *ticks)
{
    struct timespec64 elapsed;
    uint64_t delta;
    int ret;

    if (ts->tv_sec < 0 || ts->tv_nsec < 0 ||
        ts->tv_nsec >= (int64_t)NSEC_PER_SEC)
        return -EINVAL;

    elapsed.tv_sec = ts->tv_sec - (int64_t)(rt_base_ns / NSEC_PER_SEC);
    elapsed.tv_nsec = ts->tv_nsec - (int64_t)(rt_base_ns % NSEC_PER_SEC);
    if (elapsed.tv_nsec < 0) {
        --elapsed.tv_sec;
        elapsed.tv_nsec += (int64_t)NSEC_PER_SEC;
    }
    if (elapsed.tv_sec < 0) {
        *ticks = 0;
        return 0;
    }
    ret = time_ticks_from_timespec(&elapsed, &delta);
    if (ret)
        return ret;
    if (check_add_overflow(rt_base_ticks, delta, &delta))
        return -EOVERFLOW;
    *ticks = delta;
    return 0;
}
#else
int time_get_realtime(struct timespec64 *ts)
{
    (void)ts;
    return -ENODEV;
}

int time_realtime_to_ticks(const struct timespec64 *ts, uint64_t *ticks)
{
    (void)ts;
    (void)ticks;
    return -ENODEV;
}
#endif

#ifdef CONFIG_RTC_DEVICE
static int realtime_init(void)
{
    int ret = rtc_read_ns(&rt_base_ns);

    if (ret)
        return ret;
    rt_base_ticks = time_read();
    return 0;
}
MODULE_INIT(realtime_init, 80);
#endif
