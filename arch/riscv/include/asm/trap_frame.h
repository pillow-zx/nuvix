#ifndef _NUVIX_ASM_TRAP_FRAME_H
#define _NUVIX_ASM_TRAP_FRAME_H

/**
 * @file
 * @brief Trap entry register save layout.
 *
 * Assembly contract for entry.S and __trapret. Code above arch/riscv should
 * use <arch/trap.h> accessors instead of field access.
 */

#include <nuvix/compiler.h>
#include <nuvix/types.h>
#include <asm/asm_offsets.h>

/**
 * @struct trap_frame
 * @brief Complete RISC-V register image saved on every trap entry.
 *
 * The field order is an ABI between C and arch/riscv/entry.S.  Offsets must
 * match arch/riscv/include/asm/asm_offsets.h exactly because assembly saves
 * and restores registers by numeric offsets.
 *
 * @par Fields
 * - @c sepc: Saved exception PC restored into sepc before sret.
 * - @c ra: Return address register x1.
 * - @c sp: Interrupted stack pointer x2.
 * - @c gp: Global pointer x3.
 * - @c tp: Thread pointer x4 / Linux TLS register.
 * - @c t0: Temporary register x5.
 * - @c t1: Temporary register x6.
 * - @c t2: Temporary register x7.
 * - @c s0: Saved register x8 / frame pointer.
 * - @c s1: Saved register x9.
 * - @c a0: Argument/return register x10; syscall return value.
 * - @c a1: Argument register x11.
 * - @c a2: Argument register x12.
 * - @c a3: Argument register x13.
 * - @c a4: Argument register x14.
 * - @c a5: Argument register x15.
 * - @c a6: Argument register x16.
 * - @c a7: Argument register x17; Linux syscall number.
 * - @c s2: Saved register x18.
 * - @c s3: Saved register x19.
 * - @c s4: Saved register x20.
 * - @c s5: Saved register x21.
 * - @c s6: Saved register x22.
 * - @c s7: Saved register x23.
 * - @c s8: Saved register x24.
 * - @c s9: Saved register x25.
 * - @c s10: Saved register x26.
 * - @c s11: Saved register x27.
 * - @c t3: Temporary register x28.
 * - @c t4: Temporary register x29.
 * - @c t5: Temporary register x30.
 * - @c t6: Temporary register x31.
 * - @c scause: Trap cause captured from the scause CSR.
 * - @c stval: Trap value captured from the stval CSR.
 * - @c sstatus: Status image restored into sstatus before sret.
 */
struct trap_frame {
	size_t sepc;
	size_t ra;
	size_t sp;
	size_t gp;
	size_t tp;
	size_t t0;
	size_t t1;
	size_t t2;
	size_t s0;
	size_t s1;
	size_t a0;
	size_t a1;
	size_t a2;
	size_t a3;
	size_t a4;
	size_t a5;
	size_t a6;
	size_t a7;
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
	size_t t3;
	size_t t4;
	size_t t5;
	size_t t6;
	size_t scause;
	size_t stval;
	size_t sstatus;
};

#define CHECK_TF_OFFSET(member, offset)                                        \
	static_assert(offsetof(struct trap_frame, member) == (offset),         \
		      "trap_frame." #member " offset mismatch")

CHECK_TF_OFFSET(sepc, TF_SEPC);
CHECK_TF_OFFSET(ra, TF_RA);
CHECK_TF_OFFSET(sp, TF_SP);
CHECK_TF_OFFSET(gp, TF_GP);
CHECK_TF_OFFSET(tp, TF_TP);
CHECK_TF_OFFSET(t0, TF_T0);
CHECK_TF_OFFSET(t1, TF_T1);
CHECK_TF_OFFSET(t2, TF_T2);
CHECK_TF_OFFSET(s0, TF_S0);
CHECK_TF_OFFSET(s1, TF_S1);
CHECK_TF_OFFSET(a0, TF_A0);
CHECK_TF_OFFSET(a1, TF_A1);
CHECK_TF_OFFSET(a2, TF_A2);
CHECK_TF_OFFSET(a3, TF_A3);
CHECK_TF_OFFSET(a4, TF_A4);
CHECK_TF_OFFSET(a5, TF_A5);
CHECK_TF_OFFSET(a6, TF_A6);
CHECK_TF_OFFSET(a7, TF_A7);
CHECK_TF_OFFSET(s2, TF_S2);
CHECK_TF_OFFSET(s3, TF_S3);
CHECK_TF_OFFSET(s4, TF_S4);
CHECK_TF_OFFSET(s5, TF_S5);
CHECK_TF_OFFSET(s6, TF_S6);
CHECK_TF_OFFSET(s7, TF_S7);
CHECK_TF_OFFSET(s8, TF_S8);
CHECK_TF_OFFSET(s9, TF_S9);
CHECK_TF_OFFSET(s10, TF_S10);
CHECK_TF_OFFSET(s11, TF_S11);
CHECK_TF_OFFSET(t3, TF_T3);
CHECK_TF_OFFSET(t4, TF_T4);
CHECK_TF_OFFSET(t5, TF_T5);
CHECK_TF_OFFSET(t6, TF_T6);
CHECK_TF_OFFSET(scause, TF_SCAUSE);
CHECK_TF_OFFSET(stval, TF_STVAL);
CHECK_TF_OFFSET(sstatus, TF_SSTATUS);
static_assert(sizeof(struct trap_frame) == TRAP_FRAME_SIZE,
	      "trap_frame size mismatch");
static_assert(TRAP_FRAME_ALLOC_SIZE % 16 == 0,
	      "trap-frame allocation must preserve stack alignment");

#undef CHECK_TF_OFFSET

#endif
