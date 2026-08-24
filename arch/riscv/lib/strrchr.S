	.text
	.align 2

	.globl strrchr
	.type strrchr, @function
strrchr:
	li	t0, 0
	andi	a1, a1, 255
	andi	t3, a0, 7
	beqz	t3, .Lstrrchr_word_setup
.Lstrrchr_align:
	lbu	t4, 0(a0)
	beqz	t4, .Lstrrchr_zero
	bne	t4, a1, .Lstrrchr_align_next
	mv	t0, a0
.Lstrrchr_align_next:
	addi	a0, a0, 1
	andi	t3, a0, 7
	bnez	t3, .Lstrrchr_align

.Lstrrchr_word_setup:
	mv	t3, a1
	slli	t5, t3, 8
	or	t3, t3, t5
	slli	t5, t3, 16
	or	t3, t3, t5
	slli	t5, t3, 32
	or	t3, t3, t5
	li	t1, 0x0101010101010101
	li	t2, 0x8080808080808080
.Lstrrchr_word:
	ld	t4, 0(a0)
	xor	t5, t4, t3
	sub	t6, t5, t1
	not	t5, t5
	and	t6, t6, t5
	and	t6, t6, t2
	bnez	t6, .Lstrrchr_scan
	sub	t5, t4, t1
	not	t6, t4
	and	t5, t5, t6
	and	t5, t5, t2
	bnez	t5, .Lstrrchr_scan
	addi	a0, a0, 8
	j	.Lstrrchr_word

.Lstrrchr_scan:
	lbu	t4, 0(a0)
	beqz	t4, .Lstrrchr_zero
	bne	t4, a1, .Lstrrchr_scan_next
	mv	t0, a0
.Lstrrchr_scan_next:
	addi	a0, a0, 1
	j	.Lstrrchr_scan

.Lstrrchr_zero:
	beqz	a1, .Lstrrchr_found_zero
	mv	a0, t0
.Lstrrchr_found_zero:
	ret

	.size strrchr, .-strrchr
