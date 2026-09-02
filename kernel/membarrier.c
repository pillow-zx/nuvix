/* kernel/membarrier.c - cross-CPU membarrier policy and delivery */

#include <nuvix/bitops.h>
#include <nuvix/cleanup.h>
#include <nuvix/cpu.h>
#include <nuvix/errno.h>
#include <nuvix/ipi.h>
#include <nuvix/membarrier.h>
#include <nuvix/mm.h>
#include <nuvix/proc.h>
#include <nuvix/processor.h>
#include <nuvix/rseq.h>
#include <nuvix/sched.h>
#include <nuvix/task.h>
#include <uapi/membarrier.h>
#include <arch/barrier.h>

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

static bool membarrier_private_target(uint32_t id, struct mm_struct *mm,
				       bool cpu_flag, int cpu_id)
{
	if (cpu_flag && id != (uint32_t)cpu_id)
		return false;
	return sched_cpu_mm_targets(id, mm);
}

static void membarrier_send(uint32_t reasons, struct mm_struct *mm,
			    bool global, bool cpu_flag, int cpu_id)
{
	uint32_t self_id = current_cpu()->id;
	uint64_t online = cpu_online_mask();

	/* Publish the caller's prior stores before any target consumes its IPI. */
	arch_mb();
	for (uint32_t id = 0; id < nr_cpu_ids; id++) {
		if (id == self_id || !(online & BIT_U64(id)))
			continue;
		if (!global &&
		    !membarrier_private_target(id, mm, cpu_flag, cpu_id))
			continue;
		ipi_send_sync(id, (int)reasons);
	}
	/* Pair the remote completion publications with the caller's return. */
	arch_mb();
}

static int membarrier_validate_rseq_cpu(unsigned int flags, int cpu_id)
{
	if (flags && flags != MEMBARRIER_CMD_FLAG_CPU)
		return -EINVAL;
	if (flags == MEMBARRIER_CMD_FLAG_CPU &&
	    (cpu_id < 0 || !cpu_is_schedulable((uint32_t)cpu_id)))
		return -EINVAL;
	return 0;
}

ssize_t membarrier_command(int cmd, unsigned int flags, int cpu_id)
{
	struct mm_struct *mm __cleanup_with(mm_ref) = NULL;
	uint32_t registrations;
	int ret;

	if (cmd == MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ) {
		ret = membarrier_validate_rseq_cpu(flags, cpu_id);
		if (ret < 0)
			return ret;
	} else if (flags) {
		return -EINVAL;
	}

	if (current_task()->proc)
		mm = proc_mm_get(current_task()->proc);

	switch (cmd) {
	case MEMBARRIER_CMD_QUERY:
		return MEMBARRIER_SUPPORTED_MASK;
	case MEMBARRIER_CMD_GLOBAL:
	case MEMBARRIER_CMD_GLOBAL_EXPEDITED:
		membarrier_send(IPI_MEMBARRIER, NULL, true, false, 0);
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
		membarrier_send(IPI_MEMBARRIER, mm, false, false, 0);
		return 0;
	case MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE:
		if (!mm)
			return -EINVAL;
		registrations = mm_membarrier_registrations(mm);
		if (!(registrations &
		      MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE))
			return -EPERM;
		flush_icache();
		membarrier_send(IPI_MEMBARRIER | IPI_SYNC_CORE, mm, false,
				false, 0);
		return 0;
	case MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ:
		if (!mm)
			return -EINVAL;
		registrations = mm_membarrier_registrations(mm);
		if (!(registrations &
		      MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ))
			return -EPERM;
		if (!(flags & MEMBARRIER_CMD_FLAG_CPU) ||
		    cpu_id == (int)current_cpu()->id)
			rseq_request_restart(current_task(), RSEQ_EVENT_FORCE);
		membarrier_send(IPI_MEMBARRIER | IPI_RSEQ, mm, false,
				flags == MEMBARRIER_CMD_FLAG_CPU, cpu_id);
		return 0;
	case MEMBARRIER_CMD_GET_REGISTRATIONS:
		return mm ? mm_membarrier_registrations(mm) : 0;
	default:
		return -EINVAL;
	}
}
