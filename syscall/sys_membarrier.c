/*
 * syscall/sys_membarrier.c - membarrier Linux ABI compatibility wrapper
 */

#include <kernel/cpu.h>
#include <kernel/errno.h>
#include <kernel/mm.h>
#include <kernel/syscall.h>
#include <kernel/task.h>
#include <uapi/membarrier.h>
#include <kernel/processor.h>
#include <kernel/trap.h>

#define MEMBARRIER_SUPPORTED_MASK                                              \
	(MEMBARRIER_CMD_GLOBAL | MEMBARRIER_CMD_GLOBAL_EXPEDITED |             \
	 MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED |                            \
	 MEMBARRIER_CMD_PRIVATE_EXPEDITED |                                    \
	 MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED |                           \
	 MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE |                          \
	 MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE |                 \
	 MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ |                               \
	 MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ |                      \
	 MEMBARRIER_CMD_GET_REGISTRATIONS)

static inline void membarrier_full_mb(void)
{
	asm volatile("fence rw,rw" ::: "memory");
}

/*
 * SYSCALL_SUPPORT(B): membarrier
 * Current: implements single-core compatible barriers and registration state.
 * Unsupported errno: unknown commands or bad flags/cpu return -EINVAL;
 * private expedited commands without registration return -EPERM.
 * Future: keep this documented as single-core until SMP IPI/runqueue semantics
 * exist.
 */
ssize_t sys_membarrier(struct trap_frame *tf)
{
	int cmd = (int)syscall_arg(tf, 0);
	unsigned int flags = (unsigned int)syscall_arg(tf, 1);
	int cpu_id = (int)syscall_arg(tf, 2);
	struct mm_struct *mm = current_task()->proc ? current_task()->proc->mm : NULL;
	uint32_t registrations;

	switch (cmd) {
	case MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ:
		if (flags && flags != MEMBARRIER_CMD_FLAG_CPU)
			return -EINVAL;
		/* FLAG_CPU targets an online CPU; no hardcoded CPU 0. */
		if (flags == MEMBARRIER_CMD_FLAG_CPU &&
		    (cpu_id < 0 || (uint32_t)cpu_id >= nr_cpu_ids ||
		     !cpu_is_online((uint32_t)cpu_id)))
			return -EINVAL;
		break;
	default:
		if (flags)
			return -EINVAL;
	}

	switch (cmd) {
	case MEMBARRIER_CMD_QUERY:
		return MEMBARRIER_SUPPORTED_MASK;
	case MEMBARRIER_CMD_GLOBAL:
	case MEMBARRIER_CMD_GLOBAL_EXPEDITED:
		membarrier_full_mb();
		return 0;
	case MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED:
	case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED:
	case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE:
	case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ:
		if (!mm)
			return -EINVAL;
		mm_membarrier_register(mm, (uint32_t)cmd);
		membarrier_full_mb();
		return 0;
	case MEMBARRIER_CMD_PRIVATE_EXPEDITED:
		if (!mm)
			return -EINVAL;
		registrations = mm_membarrier_registrations(mm);
		if (!(registrations &
		      MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED))
			return -EPERM;
		membarrier_full_mb();
		return 0;
	case MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE:
		if (!mm)
			return -EINVAL;
		registrations = mm_membarrier_registrations(mm);
		if (!(registrations &
		      MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE))
			return -EPERM;
		flush_icache();
		return 0;
	case MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ:
		if (!mm)
			return -EINVAL;
		registrations = mm_membarrier_registrations(mm);
		if (!(registrations &
		      MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ))
			return -EPERM;
		membarrier_full_mb();
		return 0;
	case MEMBARRIER_CMD_GET_REGISTRATIONS:
		if (!mm)
			return 0;
		return mm_membarrier_registrations(mm);
	default:
		return -EINVAL;
	}
}
