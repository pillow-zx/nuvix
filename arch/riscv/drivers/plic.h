#ifndef _NUVIX_RISCV_PLIC_H
#define _NUVIX_RISCV_PLIC_H

#include <nuvix/types.h>

/* Discovery precedes allocator setup; initialization follows CPU publication. */
void plic_discover(uint32_t boot_hartid);
void plic_init(void);
/* Current CPU, IRQs disabled, before enabling external interrupts. */
void plic_cpu_init(void);
void plic_handle_irq(void);
int plic_set_threshold(uint32_t cpu, uint32_t threshold);

#endif
