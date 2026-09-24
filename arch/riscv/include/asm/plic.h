#ifndef _NUVIX_RISCV_PLIC_H
#define _NUVIX_RISCV_PLIC_H

#include <nuvix/types.h>

void plic_discover(uint32_t boot_hartid);

void plic_init(void);

void plic_cpu_init(void);

void plic_handle_irq(void);

int plic_set_threshold(uint32_t cpu, uint32_t threshold);

#endif
