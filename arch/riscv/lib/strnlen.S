	.text
	.align 2

	.globl strnlen
	.type strnlen, @function
strnlen:
	mv	t0, a0
	beqz	a1, .Lstrnlen_done
	andi	t1, a0, 7
	bnez	t1, .Lstrnlen_byte

	sltiu	t1, a1, 8
	bnez	t1, .Lstrnlen_byte
	li	t1, 0x0101010101010101
	li	t2, 0x8080808080808080
.Lstrnlen_word:
	ld	t3, 0(a0)
	sub	t4, t3, t1
	not	t5, t3
	and	t4, t4, t5
	and	t4, t4, t2
	bnez	t4, .Lstrnlen_byte
	addi	a0, a0, 8
	addi	a1, a1, -8
	sltiu	t6, a1, 8
	beqz	t6, .Lstrnlen_word

.Lstrnlen_byte:
	beqz	a1, .Lstrnlen_done
	lbu	t3, 0(a0)
	beqz	t3, .Lstrnlen_done
	addi	a0, a0, 1
	addi	a1, a1, -1
	j	.Lstrnlen_byte

.Lstrnlen_done:
	sub	a0, a0, t0
	ret

	.size strnlen, .-strnlen
