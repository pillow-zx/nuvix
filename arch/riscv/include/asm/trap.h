#ifndef _NUVIX_ASM_TRAP_H
#define _NUVIX_ASM_TRAP_H

/**
 * @file
 * @brief RISC-V scause interrupt and exception encodings.
 */

#include <nuvix/types.h>

/** @def IRQ_S_SOFT
 * @brief Supervisor software interrupt code.
 */
#define IRQ_S_SOFT   1UL
/** @def IRQ_VS_SOFT
 * @brief Virtual supervisor software interrupt code.
 */
#define IRQ_VS_SOFT  2UL
/** @def IRQ_M_SOFT
 * @brief Machine software interrupt code.
 */
#define IRQ_M_SOFT   3UL
/** @def IRQ_S_TIMER
 * @brief Supervisor timer interrupt code.
 */
#define IRQ_S_TIMER  5UL
/** @def IRQ_VS_TIMER
 * @brief Virtual supervisor timer interrupt code.
 */
#define IRQ_VS_TIMER 6UL
/** @def IRQ_M_TIMER
 * @brief Machine timer interrupt code.
 */
#define IRQ_M_TIMER  7UL
/** @def IRQ_S_EXT
 * @brief Supervisor external interrupt code.
 */
#define IRQ_S_EXT    9UL
/** @def IRQ_VS_EXT
 * @brief Virtual supervisor external interrupt code.
 */
#define IRQ_VS_EXT   10UL
/** @def IRQ_M_EXT
 * @brief Machine external interrupt code.
 */
#define IRQ_M_EXT    11UL

/** @def EXC_INST_MISALIGNED
 * @brief Instruction-address-misaligned exception code.
 */
#define EXC_INST_MISALIGNED  0UL
/** @def EXC_INST_ACCESS
 * @brief Instruction access fault exception code.
 */
#define EXC_INST_ACCESS	     1UL
/** @def EXC_INST_ILLEGAL
 * @brief Illegal instruction exception code.
 */
#define EXC_INST_ILLEGAL     2UL
/** @def EXC_BREAKPOINT
 * @brief Breakpoint exception code.
 */
#define EXC_BREAKPOINT	     3UL
/** @def EXC_LOAD_MISALIGNED
 * @brief Load-address-misaligned exception code.
 */
#define EXC_LOAD_MISALIGNED  4UL
/** @def EXC_LOAD_ACCESS
 * @brief Load access fault exception code.
 */
#define EXC_LOAD_ACCESS	     5UL
/** @def EXC_STORE_MISALIGNED
 * @brief Store/AMO address-misaligned exception code.
 */
#define EXC_STORE_MISALIGNED 6UL
/** @def EXC_STORE_ACCESS
 * @brief Store/AMO access fault exception code.
 */
#define EXC_STORE_ACCESS     7UL
/** @def EXC_ECALL_U
 * @brief Environment call from U-mode; this is the syscall trap source.
 */
#define EXC_ECALL_U	     8UL
/** @def EXC_ECALL_S
 * @brief Environment call from S-mode.
 */
#define EXC_ECALL_S	     9UL
/** @def EXC_INST_PAGE_FAULT
 * @brief Instruction page fault exception code.
 */
#define EXC_INST_PAGE_FAULT  12UL
/** @def EXC_LOAD_PAGE_FAULT
 * @brief Load page fault exception code.
 */
#define EXC_LOAD_PAGE_FAULT  13UL
/** @def EXC_STORE_PAGE_FAULT
 * @brief Store/AMO page fault exception code.
 */
#define EXC_STORE_PAGE_FAULT 15UL

/** @def SCAUSE_IRQ_FLAG
 * @brief High bit in scause; set means the low bits encode an interrupt.
 */
#define SCAUSE_IRQ_FLAG (1UL << 63)

#endif
