#ifndef _NUVIX_ASM_TRAP_H
#define _NUVIX_ASM_TRAP_H

#include <nuvix/types.h>

#define IRQ_S_SOFT   1UL
#define IRQ_VS_SOFT  2UL
#define IRQ_M_SOFT   3UL
#define IRQ_S_TIMER  5UL
#define IRQ_VS_TIMER 6UL
#define IRQ_M_TIMER  7UL
#define IRQ_S_EXT    9UL
#define IRQ_VS_EXT   10UL
#define IRQ_M_EXT    11UL

#define EXC_INST_MISALIGNED  0UL
#define EXC_INST_ACCESS	     1UL
#define EXC_INST_ILLEGAL     2UL
#define EXC_BREAKPOINT	     3UL
#define EXC_LOAD_MISALIGNED  4UL
#define EXC_LOAD_ACCESS	     5UL
#define EXC_STORE_MISALIGNED 6UL
#define EXC_STORE_ACCESS     7UL
#define EXC_ECALL_U	     8UL
#define EXC_ECALL_S	     9UL
#define EXC_INST_PAGE_FAULT  12UL
#define EXC_LOAD_PAGE_FAULT  13UL
#define EXC_STORE_PAGE_FAULT 15UL

#define SCAUSE_IRQ_FLAG (1UL << 63)

#endif
