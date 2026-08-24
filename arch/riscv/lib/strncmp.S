	.text
	.align 2

	.globl strncmp
	.type strncmp, @function
strncmp:
	beqz	a2, .Lstrncmp_equal

	xor	t0, a0, a1
	andi	t0, t0, 7
	bnez	t0, .Lstrncmp_byte
	andi	t0, a0, 7
	beqz	t0, .Lstrncmp_word_setup
.Lstrncmp_align:
	beqz	a2, .Lstrncmp_equal
	lbu	t2, 0(a0)
	lbu	t3, 0(a1)
	addi	a0, a0, 1
	addi	a1, a1, 1
	addi	a2, a2, -1
	bne	t2, t3, .Lstrncmp_diff
	beqz	t2, .Lstrncmp_equal
	andi	t0, a0, 7
	bnez	t0, .Lstrncmp_align

.Lstrncmp_word_setup:
	sltiu	t0, a2, 8
	bnez	t0, .Lstrncmp_byte
	li	t0, 0x0101010101010101
	li	t1, 0x8080808080808080
.Lstrncmp_word:
	ld	t2, 0(a0)
	ld	t3, 0(a1)
	bne	t2, t3, .Lstrncmp_byte
	sub	t4, t2, t0
	not	t5, t2
	and	t4, t4, t5
	and	t4, t4, t1
	bnez	t4, .Lstrncmp_byte
	sub	t4, t3, t0
	not	t5, t3
	and	t4, t4, t5
	and	t4, t4, t1
	bnez	t4, .Lstrncmp_byte
	addi	a0, a0, 8
	addi	a1, a1, 8
	addi	a2, a2, -8
	beqz	a2, .Lstrncmp_equal
	sltiu	t4, a2, 8
	beqz	t4, .Lstrncmp_word

.Lstrncmp_byte:
	beqz	a2, .Lstrncmp_equal
	lbu	t2, 0(a0)
	lbu	t3, 0(a1)
	addi	a0, a0, 1
	addi	a1, a1, 1
	addi	a2, a2, -1
	bne	t2, t3, .Lstrncmp_diff
	beqz	t2, .Lstrncmp_equal
	j	.Lstrncmp_byte
.Lstrncmp_equal:
	li	a0, 0
	ret
.Lstrncmp_diff:
	sub	a0, t2, t3
	ret

	.size strncmp, .-strncmp
