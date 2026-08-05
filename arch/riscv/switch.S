/*
 * arch/riscv/switch.S - cooperative context switch
 */

#include <asm/asm_offsets.h>

	.section .text.entry
	.globl switch_to
	.globl activate_pgroot
	.align 4

/*
 * activate_pgroot(a0 = target satp)
 *
 * Keep SATP activation and invalidation as one operation.  The fence is also
 * required when a recycled page-table root happens to produce the same value.
 * t0 is scratch; the helper uses the normal C ABI return register.
 */
activate_pgroot:
	csrr    t0, satp
	beq     t0, a0, .Lsatp_ready
	csrw    satp, a0
.Lsatp_ready:
	sfence.vma zero, zero
	ret

switch_to:
	sd      ra,  CTX_RA(a0)
	sd      sp,  CTX_SP(a0)
	sd      s0,  CTX_S0(a0)
	sd      s1,  CTX_S1(a0)
	sd      s2,  CTX_S2(a0)
	sd      s3,  CTX_S3(a0)
	sd      s4,  CTX_S4(a0)
	sd      s5,  CTX_S5(a0)
	sd      s6,  CTX_S6(a0)
	sd      s7,  CTX_S7(a0)
	sd      s8,  CTX_S8(a0)
	sd      s9,  CTX_S9(a0)
	sd      s10, CTX_S10(a0)
	sd      s11, CTX_S11(a0)

	/*
	 * Keep the address-space and stack handoff indivisible.  In particular,
	 * the boot idle context still uses its low-address OpenSBI stack, which is
	 * not mapped by a user page table.  After activating satp, do not touch the
	 * outgoing stack before loading the incoming one below.
	 */
	mv      a0, a2
	jal     ra, activate_pgroot

	ld      ra,  CTX_RA(a1)
	ld      sp,  CTX_SP(a1)
	ld      s0,  CTX_S0(a1)
	ld      s1,  CTX_S1(a1)
	ld      s2,  CTX_S2(a1)
	ld      s3,  CTX_S3(a1)
	ld      s4,  CTX_S4(a1)
	ld      s5,  CTX_S5(a1)
	ld      s6,  CTX_S6(a1)
	ld      s7,  CTX_S7(a1)
	ld      s8,  CTX_S8(a1)
	ld      s9,  CTX_S9(a1)
	ld      s10, CTX_S10(a1)
	ld      s11, CTX_S11(a1)
	ret
