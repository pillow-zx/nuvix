#ifndef _NUVIX_TICK_H
#define _NUVIX_TICK_H

#include <nuvix/ctypes.h>
#include <asm/time.h>

struct dtb;

/* Boot-only clock setup, before CPU startup. */
int tick_init(const struct dtb *dtb);
uint64_t time_ticks_from_ms(unsigned int ms);

static __always_inline uint64_t time_read(void)
{
    return arch_read_time();
}

/* CPU-local, with current bound and IRQs off. Returns the SBI TIME error. */
long tick_cpu_init(void);
void tick_handle(void);

#endif /* _NUVIX_TICK_H */
