	.text
	.align 2

	.globl strlen
	.type strlen, @function
strlen:
	mv	t0, a0
	andi	t1, a0, 7
	beqz	t1, .Lstrlen_word_setup
.Lstrlen_byte:
	lbu	t2, 0(a0)
	beqz	t2, .Lstrlen_done
	addi	a0, a0, 1
	j	.Lstrlen_byte

.Lstrlen_word_setup:
	li	t1, 0x0101010101010101
	li	t2, 0x8080808080808080
.Lstrlen_word:
	ld	t3, 0(a0)
	sub	t4, t3, t1
	not	t5, t3
	and	t4, t4, t5
	and	t4, t4, t2
	bnez	t4, .Lstrlen_byte
	addi	a0, a0, 8
	j	.Lstrlen_word

.Lstrlen_done:
	sub	a0, a0, t0
	ret

	.size strlen, .-strlen
