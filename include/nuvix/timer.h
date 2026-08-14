/**
 * @file timer.h
 * @brief Architecture clocksource and clockevent interfaces.
 */

#ifndef _NUVIX_TIMER_H
#define _NUVIX_TIMER_H

#include <nuvix/types.h>

/** Scheduler/accounting ticks per second. */
#define HZ              100ULL

/** QEMU virt mtime frequency in ticks per second. */
#define MTIME_FREQ      10000000ULL

/** Number of mtime ticks in one scheduler tick. */
#define CLOCKS_PER_TICK MTIME_FREQ / HZ

/** Read the architecture monotonic clocksource. */
uint64_t timer_now(void);

/** Program the next architecture clockevent deadline. */
void timer_set(uint64_t value);

/** Initialize the architecture clockevent. */
/* CPU-local Sstc programming; never resets another hart's timer. */
void timer_cpu_init(void);

/** Initialize generic per-CPU scheduler-tick clockevent state. */
/* Global: reset every CPU's clockevent slot exactly once. */
void clockevent_init(void);
/* CPU-local: activate only the current CPU's clockevent slot. */
void clockevent_cpu_init(void);

/** Handle one architecture clockevent at the supplied monotonic time. */
void clockevent_handle_irq(uint64_t now);

/** Move the current CPU clockevent earlier for a newly armed deadline. */
void clockevent_deadline_changed(uint64_t expires);

/* Boot-health: true after this CPU handled one local scheduler tick. */
bool cpu_timer_seen(uint32_t id);

#endif
