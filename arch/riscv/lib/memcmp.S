	.text
	.align 2

	.globl memcmp
	.type memcmp, @function
memcmp:
	beqz	a2, .Lmemcmp_equal

	/* Word loads are used only when both pointers have the same offset. */
	andi	t0, a0, 7
	andi	t1, a1, 7
	bne	t0, t1, .Lmemcmp_byte
	beqz	t0, .Lmemcmp_word_setup
.Lmemcmp_align:
	lbu	t2, 0(a0)
	lbu	t3, 0(a1)
	bne	t2, t3, .Lmemcmp_diff
	addi	a0, a0, 1
	addi	a1, a1, 1
	addi	a2, a2, -1
	beqz	a2, .Lmemcmp_equal
	andi	t0, a0, 7
	bnez	t0, .Lmemcmp_align

.Lmemcmp_word_setup:
	andi	t0, a2, -8
	beqz	t0, .Lmemcmp_byte
	add	t1, a0, t0
.Lmemcmp_word:
	ld	t2, 0(a0)
	ld	t3, 0(a1)
	bne	t2, t3, .Lmemcmp_byte
	addi	a0, a0, 8
	addi	a1, a1, 8
	bltu	a0, t1, .Lmemcmp_word
	andi	a2, a2, 7

.Lmemcmp_byte:
	beqz	a2, .Lmemcmp_equal
	lbu	t2, 0(a0)
	lbu	t3, 0(a1)
	addi	a0, a0, 1
	addi	a1, a1, 1
	addi	a2, a2, -1
	bne	t2, t3, .Lmemcmp_diff
	bnez	a2, .Lmemcmp_byte
.Lmemcmp_equal:
	li	a0, 0
	ret
.Lmemcmp_diff:
	sub	a0, t2, t3
	ret

	.size memcmp, .-memcmp
