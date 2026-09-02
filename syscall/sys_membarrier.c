/*
 * syscall/sys_membarrier.c - membarrier Linux ABI compatibility wrapper
 */

#include <nuvix/membarrier.h>
#include <nuvix/syscall.h>
#include <nuvix/trap.h>

/*
 * SYSCALL_SUPPORT(B): membarrier
 * Current: delegates Linux command validation, registration, target selection,
 * acknowledged IPI delivery, sync-core, and rseq restart semantics.
 * Unsupported errno: unknown commands or bad flags/cpu return -EINVAL;
 * private expedited commands without registration return -EPERM.
 * Future: keep this documented as single-core until SMP IPI/runqueue semantics
 * exist.
 */
ssize_t sys_membarrier(struct trap_frame *tf)
{
	return membarrier_command((int)syscall_arg(tf, 0),
				  (unsigned int)syscall_arg(tf, 1),
				  (int)syscall_arg(tf, 2));
}
