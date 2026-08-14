/*
 * syscall/sys_rseq.c - restartable sequence syscall ABI wrapper
 */

#include <nuvix/rseq.h>
#include <nuvix/syscall.h>
#include <nuvix/trap.h>

/*
 * SYSCALL_SUPPORT(B): rseq
 * Current: supports single-core register, unregister, resume, abort paths, and
 * documented rseq/rseq_cs flag policy.
 * Unsupported errno: unknown flags return -EINVAL; signature mismatch returns
 * -EPERM; duplicate matching registration returns -EBUSY.
 * Future: extend migrate and mm_cid semantics with SMP work.
 */
ssize_t sys_rseq(struct trap_frame *tf)
{
	struct rseq *area = (struct rseq *)syscall_arg(tf, 0);
	uint32_t len = (uint32_t)syscall_arg(tf, 1);
	int flags = (int)syscall_arg(tf, 2);
	uint32_t sig = (uint32_t)syscall_arg(tf, 3);

	return kernel_rseq(area, len, flags, sig);
}
