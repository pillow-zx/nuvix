	.text
	.align 2

	.globl __memmove
	.type __memmove, @function
__memmove:
	beq	a0, a1, .Lmemmove_return
	beqz	a2, .Lmemmove_return

	/* Keep exact end pointers for the byte tails. */
	mv	t3, a0
	add	t4, a0, a2
	add	a4, a1, a2

	/* Short copies avoid the aligned bulk paths. */
	andi	t0, a2, -16
	beqz	t0, .Lmemmove_byte

	andi	t5, t3, -8
	andi	t6, t4, -8
	beq	t5, t3, .Lmemmove_aligned_dest
	addi	t5, t5, 8
.Lmemmove_aligned_dest:

	/* Co-aligned buffers can use plain 64-bit loads and stores. */
	xor	t0, a0, a1
	andi	t1, t0, 7
	beqz	t1, .Lmemmove_coaligned

	/* Otherwise merge adjacent source words, little-endian. */
	bltu	a1, a0, .Lmemmove_misaligned_reverse
.Lmemmove_misaligned_forward:
	jal	t0, .Lmemmove_until_aligned_forward
	andi	a5, a1, 7
	slli	a6, a5, 3
	sub	a5, a1, t3
	andi	a1, a1, -8
	addi	a2, t6, 8
	not	a7, a6
	addi	a7, a7, 65

	ld	t0, 0(a1)
.Lmemmove_misaligned_forward_loop:
	ld	t1, 8(a1)
	addi	t3, t3, 16
	srl	t0, t0, a6
	sll	t2, t1, a7
	or	t2, t0, t2
	sd	t2, -16(t3)
	beq	t3, a2, .Lmemmove_misaligned_forward_done

	ld	t0, 16(a1)
	addi	a1, a1, 16
	srl	t1, t1, a6
	sll	t2, t0, a7
	or	t2, t1, t2
	sd	t2, -8(t3)
	bne	t3, t6, .Lmemmove_misaligned_forward_loop
.Lmemmove_misaligned_forward_done:
	mv	t3, t6
	add	a1, t3, a5
	j	.Lmemmove_byte_forward

.Lmemmove_misaligned_reverse:
	jal	t0, .Lmemmove_until_aligned_reverse
	andi	a5, a4, 7
	slli	a6, a5, 3
	sub	a5, a4, t4
	andi	a4, a4, -8
	addi	a2, t5, -8
	not	a7, a6
	addi	a7, a7, 65

	ld	t1, 0(a4)
.Lmemmove_misaligned_reverse_loop:
	ld	t0, -8(a4)
	addi	t4, t4, -16
	sll	t1, t1, a7
	srl	t2, t0, a6
	or	t2, t1, t2
	sd	t2, 8(t4)
	beq	t4, a2, .Lmemmove_misaligned_reverse_done

	ld	t1, -16(a4)
	addi	a4, a4, -16
	sll	t0, t0, a7
	srl	t2, t1, a6
	or	t2, t0, t2
	sd	t2, 0(t4)
	bne	t4, t5, .Lmemmove_misaligned_reverse_loop
.Lmemmove_misaligned_reverse_done:
	mv	t4, t5
	add	a4, t4, a5
	j	.Lmemmove_byte_reverse

.Lmemmove_coaligned:
	bltu	a1, a0, .Lmemmove_coaligned_reverse
.Lmemmove_coaligned_forward:
	jal	t0, .Lmemmove_until_aligned_forward
.Lmemmove_coaligned_forward_loop:
	ld	t1, 0(a1)
	addi	a1, a1, 8
	addi	t3, t3, 8
	sd	t1, -8(t3)
	bne	t3, t6, .Lmemmove_coaligned_forward_loop
	j	.Lmemmove_byte_forward

.Lmemmove_coaligned_reverse:
	jal	t0, .Lmemmove_until_aligned_reverse
.Lmemmove_coaligned_reverse_loop:
	ld	t1, -8(a4)
	addi	a4, a4, -8
	addi	t4, t4, -8
	sd	t1, 0(t4)
	bne	t4, t5, .Lmemmove_coaligned_reverse_loop
	j	.Lmemmove_byte_reverse

.Lmemmove_until_aligned_forward:
	beq	t3, t5, .Lmemmove_until_aligned_forward_return
.Lmemmove_until_aligned_forward_loop:
	lbu	t1, 0(a1)
	addi	a1, a1, 1
	addi	t3, t3, 1
	sb	t1, -1(t3)
	bne	t3, t5, .Lmemmove_until_aligned_forward_loop
.Lmemmove_until_aligned_forward_return:
	jr	t0

.Lmemmove_until_aligned_reverse:
	beq	t4, t6, .Lmemmove_until_aligned_reverse_return
.Lmemmove_until_aligned_reverse_loop:
	lbu	t1, -1(a4)
	addi	a4, a4, -1
	addi	t4, t4, -1
	sb	t1, 0(t4)
	bne	t4, t6, .Lmemmove_until_aligned_reverse_loop
.Lmemmove_until_aligned_reverse_return:
	jr	t0

.Lmemmove_byte:
	bltu	a1, a0, .Lmemmove_byte_reverse
.Lmemmove_byte_forward:
	beq	t3, t4, .Lmemmove_return
.Lmemmove_byte_forward_loop:
	lbu	t1, 0(a1)
	addi	a1, a1, 1
	addi	t3, t3, 1
	sb	t1, -1(t3)
	bne	t3, t4, .Lmemmove_byte_forward_loop
	ret

.Lmemmove_byte_reverse:
	beq	t4, t3, .Lmemmove_return
.Lmemmove_byte_reverse_loop:
	lbu	t1, -1(a4)
	addi	a4, a4, -1
	addi	t4, t4, -1
	sb	t1, 0(t4)
	bne	t4, t3, .Lmemmove_byte_reverse_loop
.Lmemmove_return:
	ret

	.size __memmove, .-__memmove

	.weak memmove
	.set memmove, __memmove
