	.text
	.align 2

	.globl __memcpy
	.type __memcpy, @function
__memcpy:
	mv	t6, a0

	sltiu	a3, a2, 128
	beqz	a3, .Lmemcpy_large

	/*
	 * Small copies dominate kernel traffic (struct assignment,
	 * fdtable moves, signal masks).  Pick the widest access width
	 * that destination, source, and length jointly allow, staying
	 * strictly aligned like the bulk path below.
	 */
	or	a3, t6, a1
	or	a3, a3, a2
	andi	a4, a3, 7
	beqz	a4, .Lmemcpy_small_dword
	andi	a4, a3, 3
	beqz	a4, .Lmemcpy_small_word
	andi	a4, a3, 1
	beqz	a4, .Lmemcpy_small_half
	j	.Lmemcpy_byte

.Lmemcpy_small_dword:
	sltiu	a3, a2, 32
	bnez	a3, .Lmemcpy_small_dw1
.Lmemcpy_small_dw4:
	ld	a4, 0(a1)
	ld	a5, 8(a1)
	ld	a6, 16(a1)
	ld	a7, 24(a1)
	addi	a1, a1, 32
	sd	a4, 0(t6)
	sd	a5, 8(t6)
	sd	a6, 16(t6)
	sd	a7, 24(t6)
	addi	t6, t6, 32
	addi	a2, a2, -32
	sltiu	a3, a2, 32
	beqz	a3, .Lmemcpy_small_dw4
.Lmemcpy_small_dw1:
	beqz	a2, .Lmemcpy_done
	ld	a4, 0(a1)
	addi	a1, a1, 8
	sd	a4, 0(t6)
	addi	t6, t6, 8
	addi	a2, a2, -8
	j	.Lmemcpy_small_dw1

.Lmemcpy_small_word:
	sltiu	a3, a2, 16
	bnez	a3, .Lmemcpy_small_w1
.Lmemcpy_small_w4:
	lw	a4, 0(a1)
	lw	a5, 4(a1)
	lw	a6, 8(a1)
	lw	a7, 12(a1)
	addi	a1, a1, 16
	sw	a4, 0(t6)
	sw	a5, 4(t6)
	sw	a6, 8(t6)
	sw	a7, 12(t6)
	addi	t6, t6, 16
	addi	a2, a2, -16
	sltiu	a3, a2, 16
	beqz	a3, .Lmemcpy_small_w4
.Lmemcpy_small_w1:
	beqz	a2, .Lmemcpy_done
	lw	a4, 0(a1)
	addi	a1, a1, 4
	sw	a4, 0(t6)
	addi	t6, t6, 4
	addi	a2, a2, -4
	j	.Lmemcpy_small_w1

.Lmemcpy_small_half:
	lhu	a4, 0(a1)
	addi	a1, a1, 2
	sh	a4, 0(t6)
	addi	t6, t6, 2
	addi	a2, a2, -2
	bnez	a2, .Lmemcpy_small_half
	ret

.Lmemcpy_large:
	andi	a3, t6, 7
	andi	a4, a1, 7
	bne	a3, a4, .Lmemcpy_byte
	beqz	a3, .Lmemcpy_bulk

	/* Bring both pointers to an 8-byte boundary. */
	andi	a3, a1, -8
	addi	a3, a3, 8
	sub	a4, a3, a1
.Lmemcpy_align:
	lbu	a5, 0(a1)
	addi	a1, a1, 1
	sb	a5, 0(t6)
	addi	t6, t6, 1
	bltu	a1, a3, .Lmemcpy_align
	sub	a2, a2, a4

.Lmemcpy_bulk:
	andi	a4, a2, -128
	beqz	a4, .Lmemcpy_tail
	add	a3, a1, a4
.Lmemcpy_bulk_loop:
	ld	a4, 0(a1)
	ld	a5, 8(a1)
	ld	a6, 16(a1)
	ld	a7, 24(a1)
	ld	t0, 32(a1)
	ld	t1, 40(a1)
	ld	t2, 48(a1)
	ld	t3, 56(a1)
	ld	t4, 64(a1)
	ld	t5, 72(a1)
	sd	a4, 0(t6)
	sd	a5, 8(t6)
	sd	a6, 16(t6)
	sd	a7, 24(t6)
	sd	t0, 32(t6)
	sd	t1, 40(t6)
	sd	t2, 48(t6)
	sd	t3, 56(t6)
	sd	t4, 64(t6)
	sd	t5, 72(t6)
	ld	a4, 80(a1)
	ld	a5, 88(a1)
	ld	a6, 96(a1)
	ld	a7, 104(a1)
	ld	t0, 112(a1)
	ld	t1, 120(a1)
	addi	a1, a1, 128
	sd	a4, 80(t6)
	sd	a5, 88(t6)
	sd	a6, 96(t6)
	sd	a7, 104(t6)
	sd	t0, 112(t6)
	sd	t1, 120(t6)
	addi	t6, t6, 128
	bltu	a1, a3, .Lmemcpy_bulk_loop
	andi	a2, a2, 127

.Lmemcpy_tail:
	beqz	a2, .Lmemcpy_done
	add	a3, a1, a2
	or	a5, a1, t6
	or	a5, a5, a3
	andi	a5, a5, 3
	bnez	a5, .Lmemcpy_byte_tail
.Lmemcpy_word_tail:
	lw	a4, 0(a1)
	addi	a1, a1, 4
	sw	a4, 0(t6)
	addi	t6, t6, 4
	bltu	a1, a3, .Lmemcpy_word_tail
	ret

.Lmemcpy_byte_tail:
	lbu	a4, 0(a1)
	addi	a1, a1, 1
	sb	a4, 0(t6)
	addi	t6, t6, 1
	bltu	a1, a3, .Lmemcpy_byte_tail
.Lmemcpy_done:
	ret

.Lmemcpy_byte:
	beqz	a2, .Lmemcpy_done
	add	a3, a1, a2
.Lmemcpy_byte_loop:
	lbu	a4, 0(a1)
	addi	a1, a1, 1
	sb	a4, 0(t6)
	addi	t6, t6, 1
	bltu	a1, a3, .Lmemcpy_byte_loop
	ret

	.size __memcpy, .-__memcpy

	.weak memcpy
	.set memcpy, __memcpy
