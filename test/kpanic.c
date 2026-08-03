#include <kernel/cpu.h>
#include <kernel/alloc.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/slab.h>
#include <kernel/spinlock.h>
#include <kernel/test.h>
#include <kernel/wait.h>

#ifdef KERNEL_PANIC_TEST

#if !defined(KPANIC_CASE_PREEMPT_UNDERFLOW) && \
	!defined(KPANIC_CASE_PREEMPT_OVERFLOW) && \
	!defined(KPANIC_CASE_SPINLOCK_WRONG_UNLOCK) && \
	!defined(KPANIC_CASE_SPINLOCK_RECURSIVE) && \
	!defined(KPANIC_CASE_SPINLOCK_CAPACITY) && \
	!defined(KPANIC_CASE_SCHEDULE_HELD_LOCK) && \
	!defined(KPANIC_CASE_SCHEDULE_PREEMPT_DISABLED) && \
	!defined(KPANIC_CASE_WAIT_HELD_LOCK) && \
	!defined(KPANIC_CASE_WAIT_PREEMPT_DISABLED) && \
	!defined(KPANIC_CASE_WAIT_HARD_IRQ) && \
	!defined(KPANIC_CASE_ALLOC_HELD_LOCK) && \
	!defined(KPANIC_CASE_ALLOC_FREE_HELD_LOCK) && \
	!defined(KPANIC_CASE_ALLOC_HARD_IRQ) && \
	!defined(KPANIC_CASE_ALLOC_SLEEPABLE_IRQ_OFF)
#error "KERNEL_PANIC_TEST requires a valid KERNEL_PANIC_CASE"
#endif

__noreturn
void kernel_panic_test_run(void)
{
#ifdef KPANIC_CASE_PREEMPT_UNDERFLOW
	pr_info("[KPANIC] case=preempt-underflow\n");
	preempt_enable();
#elif defined(KPANIC_CASE_PREEMPT_OVERFLOW)
	pr_info("[KPANIC] case=preempt-overflow\n");
	cpu_set_preempt_count(current_cpu(), INT32_MAX);
	preempt_disable();
#elif defined(KPANIC_CASE_SPINLOCK_WRONG_UNLOCK)
	spinlock_t held = SPINLOCK_INIT;
	spinlock_t other = SPINLOCK_INIT;
	irq_flags_t flags;

	pr_info("[KPANIC] case=spinlock-wrong-unlock\n");
	spin_lock_init(&held);
	spin_lock_init(&other);
	spin_lock_irqsave(&held, &flags);
	spin_unlock_irqrestore(&other, flags);
#elif defined(KPANIC_CASE_SPINLOCK_RECURSIVE)
	spinlock_t lock = SPINLOCK_INIT;
	irq_flags_t flags;

	pr_info("[KPANIC] case=spinlock-recursive\n");
	spin_lock_init(&lock);
	spin_lock_irqsave(&lock, &flags);
	spin_lock_irqsave(&lock, &flags);
#elif defined(KPANIC_CASE_SPINLOCK_CAPACITY)
	spinlock_t locks[CPU_LOCK_MAX + 1] = {0};
	irq_flags_t flags[CPU_LOCK_MAX + 1];

	pr_info("[KPANIC] case=spinlock-capacity\n");
	for (uint32_t index = 0; index < CPU_LOCK_MAX + 1; index++)
		spin_lock_init(&locks[index]);
	for (uint32_t index = 0; index < CPU_LOCK_MAX; index++)
		spin_lock_irqsave(&locks[index], &flags[index]);
	spin_lock_irqsave(&locks[CPU_LOCK_MAX], &flags[CPU_LOCK_MAX]);
#elif defined(KPANIC_CASE_SCHEDULE_HELD_LOCK)
	spinlock_t lock = SPINLOCK_INIT;
	irq_flags_t flags;

	pr_info("[KPANIC] case=schedule-held-lock\n");
	spin_lock_init(&lock);
	spin_lock_irqsave(&lock, &flags);
	schedule();
#elif defined(KPANIC_CASE_SCHEDULE_PREEMPT_DISABLED)
	pr_info("[KPANIC] case=schedule-preempt-disabled\n");
	preempt_disable();
	schedule();
#elif defined(KPANIC_CASE_WAIT_HELD_LOCK)
	spinlock_t lock = SPINLOCK_INIT;
	irq_flags_t flags;
	struct wait_deadline deadline = wait_deadline_none();
	wait_outcome_t outcome;
	int ret;

	pr_info("[KPANIC] case=wait-held-lock\n");
	spin_lock_init(&lock);
	spin_lock_irqsave(&lock, &flags);
	ret = wait_for(NULL, 0, &deadline, &outcome);
	(void)ret;
#elif defined(KPANIC_CASE_WAIT_PREEMPT_DISABLED)
	struct wait_deadline deadline = wait_deadline_none();
	wait_outcome_t outcome;
	int ret;

	pr_info("[KPANIC] case=wait-preempt-disabled\n");
	preempt_disable();
	ret = wait_for(NULL, 0, &deadline, &outcome);
	(void)ret;
#elif defined(KPANIC_CASE_WAIT_HARD_IRQ)
	struct wait_deadline deadline = wait_deadline_none();
	wait_outcome_t outcome;
	int ret;

	pr_info("[KPANIC] case=wait-hard-irq\n");
	irq_enter();
	ret = wait_for(NULL, 0, &deadline, &outcome);
	(void)ret;
#elif defined(KPANIC_CASE_ALLOC_HELD_LOCK)
	spinlock_t lock = SPINLOCK_INIT;
	irq_flags_t flags;
	void *ptr;

	pr_info("[KPANIC] case=alloc-held-lock\n");
	spin_lock_init(&lock);
	spin_lock_irqsave(&lock, &flags);
	ptr = kmalloc(16, ALLOC_NOWAIT);
	(void)ptr;
#elif defined(KPANIC_CASE_ALLOC_FREE_HELD_LOCK)
	spinlock_t lock = SPINLOCK_INIT;
	irq_flags_t flags;
	void *ptr;

	pr_info("[KPANIC] case=alloc-free-held-lock\n");
	ptr = kmalloc(16, ALLOC_NOWAIT);
	spin_lock_init(&lock);
	spin_lock_irqsave(&lock, &flags);
	kfree(ptr);
#elif defined(KPANIC_CASE_ALLOC_HARD_IRQ)
	void *ptr;

	pr_info("[KPANIC] case=alloc-hard-irq\n");
	irq_enter();
	ptr = kmalloc(16, ALLOC_NOWAIT);
	(void)ptr;
#elif defined(KPANIC_CASE_ALLOC_SLEEPABLE_IRQ_OFF)
	void *ptr;

	pr_info("[KPANIC] case=alloc-sleepable-irq-off\n");
	local_irq_disable();
	ptr = kmalloc(16, ALLOC_SLEEPABLE);
	(void)ptr;
#endif

	panic("kpanic case returned without triggering its assertion\n");
}

#endif
