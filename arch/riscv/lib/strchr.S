	.text
	.align 2

	.globl strchr
	.type strchr, @function
strchr:
	andi	a1, a1, 255
	andi	t3, a0, 7
	beqz	t3, .Lstrchr_word_setup
.Lstrchr_align:
	lbu	t3, 0(a0)
	beq	t3, a1, .Lstrchr_found
	beqz	t3, .Lstrchr_not_found
	addi	a0, a0, 1
	andi	t3, a0, 7
	bnez	t3, .Lstrchr_align

.Lstrchr_word_setup:
	mv	t0, a1
	slli	t3, t0, 8
	or	t0, t0, t3
	slli	t3, t0, 16
	or	t0, t0, t3
	slli	t3, t0, 32
	or	t0, t0, t3
	li	t1, 0x0101010101010101
	li	t2, 0x8080808080808080
.Lstrchr_word:
	ld	t3, 0(a0)
	xor	t4, t3, t0
	sub	t5, t4, t1
	not	t6, t4
	and	t5, t5, t6
	and	t5, t5, t2
	bnez	t5, .Lstrchr_scan
	sub	t4, t3, t1
	not	t5, t3
	and	t4, t4, t5
	and	t4, t4, t2
	bnez	t4, .Lstrchr_scan
	addi	a0, a0, 8
	j	.Lstrchr_word

.Lstrchr_scan:
	lbu	t3, 0(a0)
	beq	t3, a1, .Lstrchr_found
	beqz	t3, .Lstrchr_not_found
	addi	a0, a0, 1
	j	.Lstrchr_scan
.Lstrchr_found:
	ret
.Lstrchr_not_found:
	li	a0, 0
	ret

	.size strchr, .-strchr
