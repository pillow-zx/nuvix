/* kernel/rseq.c - restartable sequence registration and restart handling */

#include <nuvix/errno.h>
#include <nuvix/mm.h>
#include <nuvix/rseq.h>
#include <nuvix/task.h>
#include <nuvix/trap.h>
#include <nuvix/tools.h>
#include <nuvix/bitops.h>
#include <uapi/rseq.h>
#include <uapi/sched.h>

#define RSEQ_ORIG_SIZE 32U
#define RSEQ_NODE_ID   0U
#define RSEQ_CS_SUPPORTED_FLAGS                                             \
	(RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT | RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL | \
	 RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE)

__must_check __pure
static struct rseq *rseq_task_area(const struct task_struct *task)
{
	return task ? task->rseq.area : NULL;
}

__must_check __pure
static uint32_t rseq_task_len(const struct task_struct *task)
{
	return task ? task->rseq.len : 0;
}

__must_check __pure
static uint32_t rseq_task_sig(const struct task_struct *task)
{
	return task ? task->rseq.sig : 0;
}

__must_check __pure
static uint32_t rseq_task_cpu_id(const struct task_struct *task)
{
	return task ? task->rseq.cpu_id : UINT32_MAX;
}

static void rseq_task_set(struct task_struct *task,
			  struct rseq *area, uint32_t len, uint32_t sig,
			  uint32_t cpu_id)
{
	if (!task)
		return;

	task->rseq.area = area;
	task->rseq.len = len;
	task->rseq.sig = sig;
	task->rseq.cpu_id = cpu_id;
	atomic_set_relaxed(&task->rseq.restart_events, 0);
}

static void rseq_task_clear(struct task_struct *task)
{
	rseq_task_set(task, NULL, 0, 0, UINT32_MAX);
}

void rseq_request_restart(struct task_struct *task, uint32_t events)
{
	if (!task || !events)
		return;
	BUG_ON(events & ~RSEQ_EVENT_MASK);
	atomic_fetch_or_order(&task->rseq.restart_events, (int32_t)events,
			      ATOMIC_ORDER_RELEASE);
}

__must_check __pure
static bool rseq_area_aligned(const struct rseq *area)
{
	return IS_ALIGNED((uintptr_t)area, RSEQ_ORIG_SIZE);
}

__must_check __nonnull(1)
static int rseq_write_initial_area(struct rseq *area, uint32_t cpu_id)
{
	unsigned int zero = 0;
	unsigned long rseq_cs = 0;

	if (copy_to_user(&area->cpu_id_start, &cpu_id, sizeof(cpu_id)) != 0 ||
	    copy_to_user(&area->cpu_id, &cpu_id, sizeof(cpu_id)) != 0 ||
	    copy_to_user(&area->rseq_cs, &rseq_cs, sizeof(rseq_cs)) != 0 ||
	    copy_to_user(&area->flags, &zero, sizeof(zero)) != 0 ||
	    copy_to_user(&area->node_id, &zero, sizeof(zero)) != 0 ||
	    copy_to_user(&area->mm_cid, &cpu_id, sizeof(cpu_id)) != 0)
		return -EFAULT;

	return 0;
}

__must_check __nonnull(1)
static int rseq_write_current_ids(struct rseq *area, uint32_t cpu_id)
{
	unsigned int value;

	if (copy_to_user(&area->cpu_id_start, &cpu_id, sizeof(cpu_id)) != 0)
		return -EFAULT;
	if (copy_to_user(&area->cpu_id, &cpu_id, sizeof(cpu_id)) != 0)
		return -EFAULT;

	value = RSEQ_NODE_ID;
	if (copy_to_user(&area->node_id, &value, sizeof(value)) != 0)
		return -EFAULT;

	if (copy_to_user(&area->mm_cid, &cpu_id, sizeof(cpu_id)) != 0)
		return -EFAULT;

	return 0;
}

__must_check __nonnull(1)
static int rseq_write_unregistered_area(struct rseq *area)
{
	unsigned int cpu_id = RSEQ_CPU_ID_UNINITIALIZED;

	if (copy_to_user(&area->cpu_id, &cpu_id, sizeof(cpu_id)) != 0)
		return -EFAULT;

	return 0;
}

__must_check __nonnull(1, 2)
static int rseq_copy_cs(struct rseq_cs *dst, const struct rseq_cs *src)
{
	if ((uintptr_t)src >= TASK_SIZE)
		return -EFAULT;
	if (copy_from_user(dst, src, sizeof(*dst)) != 0)
		return -EFAULT;

	return 0;
}

__must_check
static int rseq_read_signature(uintptr_t abort_ip, uint32_t *sig)
{
	uint32_t *usig;

	if (!sig)
		return -EINVAL;
	if (abort_ip >= TASK_SIZE || abort_ip < sizeof(*sig))
		return -EFAULT;

	usig = (uint32_t *)(abort_ip - sizeof(*sig));
	if (copy_from_user(sig, usig, sizeof(*sig)) != 0)
		return -EFAULT;

	return 0;
}

__must_check __nonnull(1)
static int rseq_clear_user_cs(struct rseq *area)
{
	unsigned long zero = 0;

	if (copy_to_user(&area->rseq_cs, &zero, sizeof(zero)) != 0)
		return -EFAULT;

	return 0;
}

static bool rseq_cs_suppresses_restart(const struct rseq_cs *cs,
					uint32_t events)
{
	if (events & RSEQ_EVENT_FORCE)
		return false;
	if ((events & RSEQ_EVENT_PREEMPT) &&
	    !(cs->flags & RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT))
		return false;
	if ((events & RSEQ_EVENT_SIGNAL) &&
	    !(cs->flags & RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL))
		return false;
	if ((events & RSEQ_EVENT_MIGRATE) &&
	    !(cs->flags & RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE))
		return false;
	return events != 0;
}

__must_check __nonnull(1, 2)
static int rseq_handle_cs(struct task_struct *task, struct trap_frame *tf,
		       unsigned long csaddr, uint32_t events)
{
	struct rseq_cs cs;
	uintptr_t ip;
	uintptr_t start_ip;
	uintptr_t end;
	uintptr_t abort_ip;
	uint32_t sig;
	int ret;

	ret = rseq_copy_cs(&cs, (const struct rseq_cs *)csaddr);
	if (ret < 0)
		return ret;
	if (cs.version != 0 || (cs.flags & ~RSEQ_CS_SUPPORTED_FLAGS))
		return -EINVAL;

	ip = trap_user_pc(tf);
	start_ip = cs.start_ip;
	abort_ip = cs.abort_ip;
	end = start_ip + cs.post_commit_offset;
	if (end < start_ip || end > TASK_SIZE)
		return -EFAULT;
	if (ip < start_ip || ip >= end)
		return rseq_clear_user_cs(rseq_task_area(task));
	if (rseq_cs_suppresses_restart(&cs, events))
		return 0;

	ret = rseq_read_signature(abort_ip, &sig);
	if (ret < 0)
		return ret;
	if (sig != rseq_task_sig(task))
		return -EFAULT;

	ret = rseq_clear_user_cs(rseq_task_area(task));
	if (ret < 0)
		return ret;

	trap_set_user_pc(tf, abort_ip);
	return 0;
}

__must_check __nonnull(1, 2)
static int rseq_update_user(struct task_struct *task, struct trap_frame *tf,
			 uint32_t injected_events)
{
	struct rseq *area = rseq_task_area(task);
	unsigned long csaddr;
	uint32_t cpu_id;
	uint32_t events;
	int ret;

	if (!area)
		return 0;
	cpu_id = current_cpu()->id;
	if (rseq_task_cpu_id(task) != cpu_id)
		rseq_request_restart(task, RSEQ_EVENT_MIGRATE);
	if (injected_events)
		rseq_request_restart(task, injected_events);
	events = (uint32_t)atomic_xchg_acquire(&task->rseq.restart_events, 0);
	if (!events)
		return 0;

	ret = rseq_write_current_ids(area, cpu_id);
	if (ret < 0)
		return ret;
	task->rseq.cpu_id = cpu_id;

	if (copy_from_user(&csaddr, &area->rseq_cs, sizeof(csaddr)) != 0)
		return -EFAULT;
	if (!csaddr)
		return 0;

	return rseq_handle_cs(task, tf, csaddr, events);
}

__must_check
static int rseq_reregister(struct rseq *area, uint32_t len,
					uint32_t sig)
{
	struct task_struct *task = current_task();

	if (rseq_task_area(task) != area || rseq_task_len(task) != len)
		return -EINVAL;
	if (rseq_task_sig(task) != sig)
		return -EPERM;

	return -EBUSY;
}

__must_check
static int rseq_register(struct rseq *area, uint32_t len,
				      uint32_t sig)
{
	struct task_struct *task = current_task();
	uint32_t cpu_id = current_cpu()->id;
	int ret;

	if (!area)
		return -EFAULT;
	if (len != RSEQ_ORIG_SIZE || !rseq_area_aligned(area))
		return -EINVAL;
	if (!access_ok(area, len))
		return -EFAULT;
	if (rseq_task_area(task))
		return rseq_reregister(area, len, sig);

	ret = rseq_write_initial_area(area, cpu_id);
	if (ret < 0)
		return ret;

	rseq_task_set(task, area, len, sig, cpu_id);
	return 0;
}

__must_check
static int rseq_unregister(struct rseq *area, uint32_t len,
					int flags, uint32_t sig)
{
	struct task_struct *task = current_task();
	int ret;

	if (flags & ~RSEQ_FLAG_UNREGISTER)
		return -EINVAL;
	if (rseq_task_area(task) != area || !rseq_task_area(task))
		return -EINVAL;
	if (rseq_task_len(task) != len)
		return -EINVAL;
	if (rseq_task_sig(task) != sig)
		return -EPERM;

	ret = rseq_write_unregistered_area(area);
	if (ret < 0)
		return ret;

	rseq_task_clear(task);
	return 0;
}

ssize_t kernel_rseq(struct rseq *area, uint32_t len, int flags, uint32_t sig)
{
	if (flags & RSEQ_FLAG_UNREGISTER)
		return rseq_unregister(area, len, flags, sig);
	if (flags)
		return -EINVAL;

	return rseq_register(area, len, sig);
}

void rseq_execve(struct task_struct *task)
{
	rseq_task_clear(task);
}

void rseq_clone(struct task_struct *child, const struct task_struct *parent,
		unsigned long flags)
{
	if (flags & CLONE_VM) {
		rseq_task_clear(child);
		return;
	}

	rseq_task_set(child, rseq_task_area(parent), rseq_task_len(parent),
		      rseq_task_sig(parent), rseq_task_cpu_id(parent));
	rseq_request_restart(child, RSEQ_EVENT_PREEMPT);
}

void rseq_sched_switch(struct task_struct *prev)
{
	if (!prev || !rseq_task_area(prev) || !task_trap_frome_user(prev))
		return;

	rseq_request_restart(prev, RSEQ_EVENT_PREEMPT);
}

int rseq_resume_user(struct trap_frame *tf)
{
	return rseq_update_user(current_task(), tf, 0);
}

int rseq_signal_deliver(struct trap_frame *tf)
{
	return rseq_update_user(current_task(), tf, RSEQ_EVENT_SIGNAL);
}
