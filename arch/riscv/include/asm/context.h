#ifndef _NUVIX_ASM_CONTEXT_H
#define _NUVIX_ASM_CONTEXT_H

/**
 * @file
 * @brief switch.S saved context layout.
 */

#include <nuvix/types.h>

/**
 * @struct context
 * @brief Callee-saved kernel context switched by switch.S.
 *
 * The layout is consumed by arch/riscv/switch.S.  It only contains registers
 * that must survive a cooperative kernel context switch; trap-time volatile
 * state lives in struct trap_frame instead.
 *
 * @par Fields
 * - @c ra: Resume PC used when switch.S returns into this context.
 * - @c sp: Kernel stack pointer for the resumed task.
 * - @c s0: Callee-saved register x8 / frame pointer.
 * - @c s1: Callee-saved register x9.
 * - @c s2: Callee-saved register x18.
 * - @c s3: Callee-saved register x19.
 * - @c s4: Callee-saved register x20.
 * - @c s5: Callee-saved register x21.
 * - @c s6: Callee-saved register x22.
 * - @c s7: Callee-saved register x23.
 * - @c s8: Callee-saved register x24.
 * - @c s9: Callee-saved register x25.
 * - @c s10: Callee-saved register x26.
 * - @c s11: Callee-saved register x27.
 */
struct context {
	size_t ra;
	size_t sp;
	size_t s0;
	size_t s1;
	size_t s2;
	size_t s3;
	size_t s4;
	size_t s5;
	size_t s6;
	size_t s7;
	size_t s8;
	size_t s9;
	size_t s10;
	size_t s11;
};

#endif
