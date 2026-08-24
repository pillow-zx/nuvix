	.text
	.align 2

	.globl strcmp
	.type strcmp, @function
strcmp:
	/* Only co-aligned pointers can use naturally aligned word loads. */
	xor	t0, a0, a1
	andi	t0, t0, 7
	bnez	t0, .Lstrcmp_byte
	andi	t0, a0, 7
	beqz	t0, .Lstrcmp_word_setup
.Lstrcmp_align:
	lbu	t2, 0(a0)
	lbu	t3, 0(a1)
	addi	a0, a0, 1
	addi	a1, a1, 1
	bne	t2, t3, .Lstrcmp_diff
	beqz	t2, .Lstrcmp_equal
	andi	t0, a0, 7
	bnez	t0, .Lstrcmp_align

.Lstrcmp_word_setup:
	li	t0, 0x0101010101010101
	li	t1, 0x8080808080808080
.Lstrcmp_word:
	ld	t2, 0(a0)
	ld	t3, 0(a1)
	bne	t2, t3, .Lstrcmp_byte
	sub	t4, t2, t0
	not	t5, t2
	and	t4, t4, t5
	and	t4, t4, t1
	bnez	t4, .Lstrcmp_byte
	addi	a0, a0, 8
	addi	a1, a1, 8
	j	.Lstrcmp_word

.Lstrcmp_byte:
	lbu	t2, 0(a0)
	lbu	t3, 0(a1)
	addi	a0, a0, 1
	addi	a1, a1, 1
	bne	t2, t3, .Lstrcmp_diff
	bnez	t2, .Lstrcmp_byte
.Lstrcmp_equal:
	li	a0, 0
	ret
.Lstrcmp_diff:
	sub	a0, t2, t3
	ret

	.size strcmp, .-strcmp
