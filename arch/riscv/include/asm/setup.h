#ifndef _NUVIX_RISCV_SETUP_H
#define _NUVIX_RISCV_SETUP_H

#include <nuvix/atomic.h>
#include <nuvix/types.h>

void dtb_init(paddr_t address);
void arch_cpu_discover(uint64_t boot_hartid);
void riscv_timer_init(uint32_t frequency, bool sstc);
extern atomic64_t pt_boot_token;

#endif
