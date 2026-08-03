#include <kernel/alloc.h>
#include <kernel/buddy.h>
#include <kernel/sched.h>
#include <kernel/slab.h>
#include <kernel/test.h>
#include <kernel/task.h>
#include <kernel/vmalloc.h>

int test_alloc_context_guards(void)
{
	struct task_struct *saved_task = current_task();
	spinlock_t lock = SPINLOCK_INIT;
	irq_flags_t saved_flags = local_irq_save();
	irq_flags_t lock_flags;
	int saved_preempt = preempt_count();
	void *ptr = NULL;
	void *page = NULL;
	void *area = NULL;

	TEST_BEGIN("alloc: context guards and mode propagation");
	{
		local_irq_enable();
		TEST_ASSERT(in_task_context());
		TEST_ASSERT(alloc_can_allocate(ALLOC_NOWAIT));
		TEST_ASSERT(alloc_can_allocate(ALLOC_SLEEPABLE));
		TEST_ASSERT(alloc_can_free());
		TEST_ASSERT(!alloc_can_allocate((enum alloc_mode)99));

		ptr = kzalloc(32, ALLOC_SLEEPABLE);
		TEST_ASSERT_NOT_NULL(ptr);
		for (size_t i = 0; i < 32; i++)
			TEST_ASSERT_EQ(((uint8_t *)ptr)[i], (uint8_t)0);
		kfree(ptr);
		ptr = kmalloc_array(2, 16, ALLOC_SLEEPABLE);
		TEST_ASSERT_NOT_NULL(ptr);
		kfree(ptr);
		ptr = NULL;
		page = get_free_page(0, ALLOC_SLEEPABLE);
		TEST_ASSERT_NOT_NULL(page);
		free_page(page, 0);
		page = NULL;
		area = vmalloc(PAGE_SIZE, ALLOC_SLEEPABLE);
		TEST_ASSERT_NOT_NULL(area);
		vfree(area);
		area = NULL;

		local_irq_disable();
		TEST_ASSERT(alloc_can_allocate(ALLOC_NOWAIT));
		TEST_ASSERT(!alloc_can_allocate(ALLOC_SLEEPABLE));
		TEST_ASSERT(alloc_can_free());
		ptr = kmalloc(16, ALLOC_NOWAIT);
		TEST_ASSERT_NOT_NULL(ptr);
		kfree(ptr);
		ptr = NULL;

		local_irq_enable();
		preempt_disable();
		TEST_ASSERT(alloc_can_allocate(ALLOC_NOWAIT));
		TEST_ASSERT(!alloc_can_allocate(ALLOC_SLEEPABLE));
		preempt_enable();

		irq_enter();
		TEST_ASSERT(!in_task_context());
		TEST_ASSERT(!alloc_can_allocate(ALLOC_NOWAIT));
		TEST_ASSERT(!alloc_can_allocate(ALLOC_SLEEPABLE));
		TEST_ASSERT(!alloc_can_free());
		irq_exit();

		spin_lock_init(&lock);
		spin_lock_irqsave(&lock, &lock_flags);
		TEST_ASSERT(spinlock_held());
		TEST_ASSERT(!alloc_can_allocate(ALLOC_NOWAIT));
		TEST_ASSERT(!alloc_can_allocate(ALLOC_SLEEPABLE));
		TEST_ASSERT(!alloc_can_free());
		spin_unlock_irqrestore(&lock, lock_flags);
		TEST_ASSERT(alloc_can_free());

		set_current_task(&idle_task);
		local_irq_enable();
		TEST_ASSERT(!in_task_context());
		TEST_ASSERT(alloc_can_allocate(ALLOC_NOWAIT));
		TEST_ASSERT(!alloc_can_allocate(ALLOC_SLEEPABLE));
		set_current_task(saved_task);
	}
	TEST_END("alloc: context guards and mode propagation");
	goto cleanup;
fail:
	TEST_FAIL("alloc: context guards and mode propagation", "see above");
cleanup:
	if (area)
		vfree(area);
	if (page)
		free_page(page, 0);
	if (ptr)
		kfree(ptr);
	set_current_task(saved_task);
	while (preempt_count() > saved_preempt)
		preempt_enable();
	local_irq_restore(saved_flags);

	return __test_ret;
}
