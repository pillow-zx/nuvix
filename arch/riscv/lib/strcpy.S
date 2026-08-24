	.text
	.align 2

	.globl strcpy
	.type strcpy, @function
strcpy:
	mv	t0, a0
	xor	t1, a0, a1
	andi	t1, t1, 7
	bnez	t1, .Lstrcpy_byte
	andi	t1, a0, 7
	beqz	t1, .Lstrcpy_word_setup
.Lstrcpy_align:
	lbu	t2, 0(a1)
	sb	t2, 0(a0)
	addi	a0, a0, 1
	addi	a1, a1, 1
	beqz	t2, .Lstrcpy_done
	andi	t1, a0, 7
	bnez	t1, .Lstrcpy_align

.Lstrcpy_word_setup:
	li	t1, 0x0101010101010101
	li	t2, 0x8080808080808080
.Lstrcpy_word:
	ld	t3, 0(a1)
	sub	t4, t3, t1
	not	t5, t3
	and	t4, t4, t5
	and	t4, t4, t2
	bnez	t4, .Lstrcpy_byte
	sd	t3, 0(a0)
	addi	a0, a0, 8
	addi	a1, a1, 8
	j	.Lstrcpy_word

.Lstrcpy_byte:
	lbu	t3, 0(a1)
	sb	t3, 0(a0)
	addi	a0, a0, 1
	addi	a1, a1, 1
	bnez	t3, .Lstrcpy_byte
.Lstrcpy_done:
	mv	a0, t0
	ret

	.size strcpy, .-strcpy
