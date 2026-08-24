	.text
	.align 2

	.globl strncpy
	.type strncpy, @function
strncpy:
	mv	t0, a0
	beqz	a2, .Lstrncpy_done

	xor	t1, a0, a1
	andi	t1, t1, 7
	bnez	t1, .Lstrncpy_byte
	andi	t1, a0, 7
	bnez	t1, .Lstrncpy_byte
	sltiu	t1, a2, 8
	bnez	t1, .Lstrncpy_byte
	li	t1, 0x0101010101010101
	li	t2, 0x8080808080808080
.Lstrncpy_word:
	ld	t3, 0(a1)
	sub	t4, t3, t1
	not	t5, t3
	and	t4, t4, t5
	and	t4, t4, t2
	bnez	t4, .Lstrncpy_byte
	sd	t3, 0(a0)
	addi	a0, a0, 8
	addi	a1, a1, 8
	addi	a2, a2, -8
	sltiu	t4, a2, 8
	beqz	t4, .Lstrncpy_word

.Lstrncpy_byte:
	beqz	a2, .Lstrncpy_done
	lbu	t3, 0(a1)
	sb	t3, 0(a0)
	addi	a0, a0, 1
	addi	a2, a2, -1
	beqz	t3, .Lstrncpy_zero_fill
	addi	a1, a1, 1
	j	.Lstrncpy_byte

.Lstrncpy_zero_fill:
	beqz	a2, .Lstrncpy_done
	sb	zero, 0(a0)
	addi	a0, a0, 1
	addi	a2, a2, -1
	j	.Lstrncpy_zero_fill

.Lstrncpy_done:
	mv	a0, t0
	ret

	.size strncpy, .-strncpy
