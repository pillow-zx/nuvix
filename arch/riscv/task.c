#include <nuvix/string.h>
#include <asm/asm_offsets.h>
#include <asm/mmu.h>

#include <nuvix/sched.h>
#include <nuvix/mm.h>
#include <nuvix/pgtable.h>
#include <nuvix/task.h>
#include <nuvix/tools.h>
#include <uapi/sched.h>
#include <nuvix/trap.h>

static_assert(ARCH_KSTACK_SIZE == TASK_KSTACK_SIZE,
	      "entry.S __trapret kstack arithmetic is out of sync");
static_assert((ARCH_KSTACK_SIZE - TRAP_FRAME_ALLOC_SIZE) % 16 == 0,
	      "kernel trap-frame allocation must preserve stack alignment");

static_assert(offsetof(struct task_struct, arch.kstack) == TASK_KSTACK,
		      "TASK_KSTACK offset in entry.S out of sync with task_struct");

__must_check __pure __nonnull(1) __returns_nonnull
struct trap_frame *task_kernel_tf(struct task_struct *task)
{
	uintptr_t frame = (uintptr_t)task->arch.kstack + KSTACK_SIZE -
			  TRAP_FRAME_ALLOC_SIZE;

	return (struct trap_frame *)frame;
}

__must_check
void *task_kstack_take(struct task_struct *task)
{
	void *kstack;

	if (!task)
		return NULL;
	kstack = task->arch.kstack;
	task->arch.kstack = NULL;
	return kstack;
}

struct task_struct *switch_to(struct context *prev, struct context *next,
			      uintptr_t next_satp,
			      struct task_struct *outgoing);

__nonnull(1)
static inline void trap_set_kthread_frame(struct trap_frame *tf, uintptr_t pc, uintptr_t arg0)
{
	memset(tf, 0, sizeof(*tf));
	tf->sepc = pc;
	tf->a0 = arg0;
	tf->sstatus = SSTATUS_SPP | SSTATUS_SPIE;
}

__nonnull(1, 2)
static inline void trap_clone_frame(struct trap_frame *dst, const struct trap_frame *src)
{
	memcpy(dst, src, sizeof(*dst));
}

__nonnull(1)
static inline void trap_set_clone_return(struct trap_frame *tf)
{
	tf->a0 = 0;
}

__nonnull(1)
static inline void trap_set_tls(struct trap_frame *tf, uintptr_t tls)
{
	tf->tp = tls;
}

__nonnull(1)
static inline void trap_setup_user_return(struct trap_frame *tf, uintptr_t pc, uintptr_t sp)
{
	tf->sepc = pc;
	tf->sp = sp;
	tf->sstatus = SSTATUS_SPIE;
}

void arch_task_init(struct task_struct *task)
{
	task->arch.ctx.ra = 0;
	task->arch.ctx.sp = 0;
	task->arch.tf = NULL;
}

void task_setup_kthread(struct task_struct *task, void (*fn)(void *),
				   void *arg)
{
	struct trap_frame *tf = task_kernel_tf(task);

	trap_set_kthread_frame(tf, (uintptr_t)fn, (uintptr_t)arg);

	task->arch.tf = tf;
	task->arch.ctx.ra = (size_t)sched_first_dispatch;
	task->arch.ctx.sp = (size_t)tf;
}

void task_setup_clone_frame(struct task_struct *child,
				 const struct trap_frame *parent_tf,
				 unsigned long flags, uintptr_t child_stack,
				 uintptr_t tls)
{
	struct trap_frame *child_tf = task_kernel_tf(child);

	trap_clone_frame(child_tf, parent_tf);
	child->arch.fpu = current_task()->arch.fpu;
	trap_set_clone_return(child_tf);
	if (child_stack != 0)
		trap_set_user_sp(child_tf, child_stack);
	if (flags & CLONE_SETTLS)
		trap_set_tls(child_tf, tls);

	child->arch.tf = child_tf;
	child->arch.ctx.ra = (size_t)sched_first_dispatch;
	child->arch.ctx.sp = (size_t)child_tf;
}

struct task_struct *arch_task_switch(struct task_struct *prev,
				     struct task_struct *next,
				     struct mm_struct *mm)
{
	return switch_to(&prev->arch.ctx, &next->arch.ctx, mm ? mm_pgtable_token(mm) : kernel_satp, prev);
}

bool task_trap_frome_user(const struct task_struct *task)
{
	const struct trap_frame *tf = task->arch.tf;

	return tf && trap_frame_from_user(tf);
}

void activate_mm(struct mm_struct *mm)
{
	active_pgtable(mm ? mm_pgtable_token(mm) : kernel_satp);
}

void arch_task_exec(struct task_struct *task, struct trap_frame *tf,
		    uintptr_t entry, uintptr_t sp)
{
	task->arch.tf = tf;
	memset(tf, 0, sizeof(*tf));
	memset(&task->arch.fpu, 0, sizeof(task->arch.fpu));
	trap_setup_user_return(tf, entry, sp);
}
