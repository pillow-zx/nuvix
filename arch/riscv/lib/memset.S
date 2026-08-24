	.text
	.align 2

	.globl __memset
	.type __memset, @function
__memset:
	mv	t0, a0

	/* Small fills are cheaper without alignment setup. */
	sltiu	a3, a2, 16
	bnez	a3, .Lmemset_tail

	/* Align the destination before switching to 64-bit stores. */
	addi	a3, t0, 7
	andi	a3, a3, -8
	beq	a3, t0, .Lmemset_broadcast
	sub	a4, a3, t0
.Lmemset_align:
	sb	a1, 0(t0)
	addi	t0, t0, 1
	bltu	t0, a3, .Lmemset_align
	sub	a2, a2, a4

.Lmemset_broadcast:
	andi	a1, a1, 255
	slli	a3, a1, 8
	or	a1, a1, a3
	slli	a3, a1, 16
	or	a1, a1, a3
	slli	a3, a1, 32
	or	a1, a1, a3

	/* 32 stores per iteration, with a Duff-style short first pass. */
	andi	a4, a2, -8
	add	a3, t0, a4
	andi	a4, a4, 248
	beqz	a4, .Lmemset_bulk_loop
	neg	a4, a4
	addi	a4, a4, 256
	sub	t0, t0, a4
	lla	a5, .Lmemset_bulk_loop
	srli	a4, a4, 1
	add	a5, a5, a4
	jr	a5

.Lmemset_bulk_loop:
	sd	a1, 0(t0)
	sd	a1, 8(t0)
	sd	a1, 16(t0)
	sd	a1, 24(t0)
	sd	a1, 32(t0)
	sd	a1, 40(t0)
	sd	a1, 48(t0)
	sd	a1, 56(t0)
	sd	a1, 64(t0)
	sd	a1, 72(t0)
	sd	a1, 80(t0)
	sd	a1, 88(t0)
	sd	a1, 96(t0)
	sd	a1, 104(t0)
	sd	a1, 112(t0)
	sd	a1, 120(t0)
	sd	a1, 128(t0)
	sd	a1, 136(t0)
	sd	a1, 144(t0)
	sd	a1, 152(t0)
	sd	a1, 160(t0)
	sd	a1, 168(t0)
	sd	a1, 176(t0)
	sd	a1, 184(t0)
	sd	a1, 192(t0)
	sd	a1, 200(t0)
	sd	a1, 208(t0)
	sd	a1, 216(t0)
	sd	a1, 224(t0)
	sd	a1, 232(t0)
	sd	a1, 240(t0)
	sd	a1, 248(t0)
	addi	t0, t0, 256
	bltu	t0, a3, .Lmemset_bulk_loop
	andi	a2, a2, 7

.Lmemset_tail:
	beqz	a2, .Lmemset_done
	add	a3, t0, a2
.Lmemset_tail_loop:
	sb	a1, 0(t0)
	addi	t0, t0, 1
	bltu	t0, a3, .Lmemset_tail_loop
.Lmemset_done:
	ret

	.size __memset, .-__memset

	.weak memset
	.set memset, __memset
