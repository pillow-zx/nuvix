/*
 * arch/riscv/uaccess.S - fault-safe user u32 accesses
 */

#define SSTATUS_SUM 0x40000
#define EAGAIN      11
#define EFAULT      14
#define EINVAL      22

	.text

/* a0: user address, a1: kernel destination, return a0: errno */
.macro USER_U32_LOAD name, order
	.section .text.\name,"ax",@progbits
	.align 2
	.globl \name
	.type \name, @function
\name:
	andi    t0, a0, 3
	bnez    t0, \name\()_unaligned
	csrr    t1, sstatus
	li      t2, SSTATUS_SUM
	or      t2, t1, t2
	csrw    sstatus, t2
.ifc \order, seq_cst
	fence   rw, rw
.endif
1:
	lwu     t0, 0(a0)
.ifc \order, acquire
	fence   r, rw
.else
.ifc \order, seq_cst
	fence   rw, rw
.endif
.endif
	csrw    sstatus, t1
	sw      t0, 0(a1)
	li      a0, 0
	ret

\name\()_unaligned:
	li      a0, -EINVAL
	ret

\name\()_fault:
	csrw    sstatus, t1
	li      a0, -EFAULT
	ret

	.pushsection __ex_table, "a"
	.balign 8
	.dword  1b, \name\()_fault
	.popsection
	.size \name, .-\name
.endm

/* a0: user address, a1: value, return a0: errno */
.macro USER_U32_STORE name, order
	.section .text.\name,"ax",@progbits
	.align 2
	.globl \name
	.type \name, @function
\name:
	andi    t0, a0, 3
	bnez    t0, \name\()_unaligned
	csrr    t1, sstatus
	li      t2, SSTATUS_SUM
	or      t2, t1, t2
.ifc \order, release
	fence   rw, w
.else
.ifc \order, seq_cst
	fence   rw, rw
.endif
.endif
	csrw    sstatus, t2
1:
	sw      a1, 0(a0)
.ifc \order, seq_cst
	fence   rw, rw
.endif
	csrw    sstatus, t1
	li      a0, 0
	ret

\name\()_unaligned:
	li      a0, -EINVAL
	ret

\name\()_fault:
	csrw    sstatus, t1
	li      a0, -EFAULT
	ret

	.pushsection __ex_table, "a"
	.balign 8
	.dword  1b, \name\()_fault
	.popsection
	.size \name, .-\name
.endm

/* a0: user address, a1: expected, a2: desired, a3: kernel observed output */
.macro USER_U32_CMPXCHG name, lr_order, sc_order
	.section .text.\name,"ax",@progbits
	.align 2
	.globl \name
	.type \name, @function
\name:
	andi    t0, a0, 3
	bnez    t0, \name\()_unaligned
	csrr    t3, sstatus
	li      t4, SSTATUS_SUM
	or      t4, t3, t4
	slli    t5, a1, 32
	srli    t5, t5, 32
.ifc \lr_order, seq_cst
	fence   rw, rw
.endif
	csrw    sstatus, t4

\name\()_retry:
1:
	lr.w\lr_order t0, (a0)
	slli    t6, t0, 32
	srli    t6, t6, 32
	bne     t6, t5, \name\()_mismatch
2:
	sc.w\sc_order t1, a2, (a0)
	bnez    t1, \name\()_retry
.ifc \lr_order, seq_cst
	fence   rw, rw
.endif
	csrw    sstatus, t3
	sw      t6, 0(a3)
	li      a0, 0
	ret

\name\()_mismatch:
	csrw    sstatus, t3
	sw      t6, 0(a3)
	li      a0, -EAGAIN
	ret

\name\()_unaligned:
	li      a0, -EINVAL
	ret

\name\()_fault:
	csrw    sstatus, t3
	li      a0, -EFAULT
	ret

	.pushsection __ex_table, "a"
	.balign 8
	.dword  1b, \name\()_fault
	.dword  2b, \name\()_fault
	.popsection
	.size \name, .-\name
.endm

USER_U32_LOAD riscv_user_u32_load_relaxed, relaxed
USER_U32_LOAD riscv_user_u32_load_acquire, acquire
USER_U32_LOAD riscv_user_u32_load_seq_cst, seq_cst

USER_U32_STORE riscv_user_u32_store_relaxed, relaxed
USER_U32_STORE riscv_user_u32_store_release, release
USER_U32_STORE riscv_user_u32_store_seq_cst, seq_cst

USER_U32_CMPXCHG riscv_user_u32_cmpxchg_relaxed, ,
USER_U32_CMPXCHG riscv_user_u32_cmpxchg_acquire, .aq,
USER_U32_CMPXCHG riscv_user_u32_cmpxchg_release, , .rl
USER_U32_CMPXCHG riscv_user_u32_cmpxchg_acq_rel, .aq, .rl
USER_U32_CMPXCHG riscv_user_u32_cmpxchg_seq_cst, .aqrl, .aqrl
