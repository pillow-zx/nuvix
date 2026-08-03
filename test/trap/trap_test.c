#include <kernel/test.h>
#include <kernel/irq.h>
#include <kernel/sched.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/time.h>
#include <kernel/timer.h>
#include <kernel/trap.h>
#include <uapi/signal.h>

struct irq_context_test_state {
	bool called;
	bool in_handler;
	uint32_t nesting;
	int preempt_count;
};

static void irq_context_test_callback(struct ktimer *timer, void *arg)
{
	struct irq_context_test_state *state = arg;

	(void)timer;
	state->called = true;
	state->in_handler = in_irq();
	state->nesting = irq_nesting();
	state->preempt_count = cpu_preempt_count(current_cpu());
}

int test_trap_frame_layout(void)
{
	TEST_BEGIN("trap: frame layout");
	{
		TEST_ASSERT_EQ(trap_frame_size(),
			       (size_t)(35 * sizeof(size_t)));
		TEST_ASSERT_EQ(TRAP_FRAME_ALLOC_SIZE,
			       (size_t)(36 * sizeof(size_t)));
		TEST_ASSERT_EQ(TRAP_FRAME_ALLOC_SIZE % 16, 0UL);

		struct trap_frame tf = {
			.sstatus = SSTATUS_FS_MASK,
		};

		trap_disable_user_fpu(&tf);
		TEST_ASSERT_EQ(tf.sstatus & SSTATUS_FS_MASK, 0UL);
	}
	TEST_END("trap: frame layout");
	return __test_ret;
fail:
	TEST_FAIL("trap: frame layout", "see above");

	return __test_ret;
}

int test_trap_from_user(void)
{
	TEST_BEGIN("trap: arch_from_user helper");
	{
		struct trap_frame tf;

		memset(&tf, 0, sizeof(tf));

		trap_setup_user_return(&tf, 0, 0);
		TEST_ASSERT(trap_frame_from_user(&tf) == true);

		trap_set_kernel_return(&tf, 0);
		TEST_ASSERT(trap_frame_from_user(&tf) == false);

		trap_setup_user_return(&tf, 0, 0);
		TEST_ASSERT(trap_frame_from_user(&tf) == true);
	}
	TEST_END("trap: arch_from_user helper");
	return __test_ret;
fail:
	TEST_FAIL("trap: arch_from_user helper", "see above");

	return __test_ret;
}

int test_trap_context_layout(void)
{
	TEST_BEGIN("trap: context layout");
	{
		TEST_ASSERT_EQ(trap_context_size(),
			       (size_t)(14 * sizeof(size_t)));
	}
	TEST_END("trap: context layout");
	return __test_ret;
fail:
	TEST_FAIL("trap: context layout", "see above");

	return __test_ret;
}

int test_trap_irq_codes(void)
{
	TEST_BEGIN("trap: IRQ/exception codes");
	{
		TEST_ASSERT_EQ(IRQ_S_SOFT, 1UL);
		TEST_ASSERT_EQ(IRQ_S_TIMER, 5UL);
		TEST_ASSERT_EQ(IRQ_S_EXT, 9UL);

		TEST_ASSERT_EQ(EXC_INST_ILLEGAL, 2UL);
		TEST_ASSERT_EQ(EXC_ECALL_U, 8UL);
		TEST_ASSERT_EQ(EXC_ECALL_S, 9UL);
		TEST_ASSERT_EQ(EXC_LOAD_PAGE_FAULT, 13UL);
		TEST_ASSERT_EQ(EXC_STORE_PAGE_FAULT, 15UL);

		TEST_ASSERT_EQ(SCAUSE_IRQ_FLAG, (1UL << 63));
	}
	TEST_END("trap: IRQ/exception codes");
	return __test_ret;
fail:
	TEST_FAIL("trap: IRQ/exception codes", "see above");

	return __test_ret;
}

int test_irq_nesting_context(void)
{
	struct irq_context_test_state state = {0};
	struct trap_frame tf = {0};
	struct trap_frame *saved_tf = task_trap_frame(current_task());
	struct ktimer timer = {0};
	irq_flags_t saved_flags = local_irq_save();
	int saved_preempt_count = cpu_preempt_count(current_cpu());
	uint32_t saved_nesting = irq_nesting();

	TEST_BEGIN("irq: nesting is independent from preempt count");
	{
		TEST_ASSERT_EQ(saved_nesting, (uint32_t)0);
		TEST_ASSERT_EQ(saved_preempt_count, 0);
		TEST_ASSERT_EQ(preempt_count(), 0);
		TEST_ASSERT(sched_context_can_schedule());
		TEST_ASSERT(wait_context_can_sleep());

		local_irq_enable();
		irq_enter();
		TEST_ASSERT(in_irq());
		TEST_ASSERT(!sched_context_can_schedule());
		TEST_ASSERT(!wait_context_can_sleep());
		TEST_ASSERT_EQ(irq_nesting(), (uint32_t)1);
		TEST_ASSERT_EQ(cpu_preempt_count(current_cpu()), 0);
		TEST_ASSERT(!irqs_disabled());

		preempt_disable();
		TEST_ASSERT_EQ(cpu_preempt_count(current_cpu()), 1);
		TEST_ASSERT(!sched_context_can_schedule());
		TEST_ASSERT(!wait_context_can_sleep());
		TEST_ASSERT_EQ(irq_nesting(), (uint32_t)1);

		irq_enter();
		TEST_ASSERT_EQ(irq_nesting(), (uint32_t)2);
		TEST_ASSERT_EQ(cpu_preempt_count(current_cpu()), 1);
		TEST_ASSERT(in_irq());

		irq_exit();
		preempt_enable();
		irq_exit();
		TEST_ASSERT(!in_irq());
		TEST_ASSERT(sched_context_can_schedule());
		TEST_ASSERT(wait_context_can_sleep());
		TEST_ASSERT_EQ(irq_nesting(), (uint32_t)0);
		TEST_ASSERT_EQ(cpu_preempt_count(current_cpu()), 0);
		TEST_ASSERT(!irqs_disabled());

		local_irq_disable();
		irq_enter();
		irq_exit();
		TEST_ASSERT(irqs_disabled());

		ktimer_init(&timer, irq_context_test_callback, &state);
		TEST_ASSERT_EQ(ktimer_arm(&timer, timer_now(), 0), 0);
		trap_set_kernel_return(&tf, 0);
		tf.scause = SCAUSE_IRQ_FLAG | IRQ_S_TIMER;
		trap_handler(&tf);

		TEST_ASSERT(state.called);
		TEST_ASSERT(state.in_handler);
		TEST_ASSERT_EQ(state.nesting, (uint32_t)1);
		TEST_ASSERT_EQ(state.preempt_count, 0);
		TEST_ASSERT_EQ(irq_nesting(), (uint32_t)0);
		TEST_ASSERT(!in_irq());
	}
	TEST_END("irq: nesting is independent from preempt count");
	goto cleanup;
fail:
	TEST_FAIL("irq: nesting is independent from preempt count",
		  "see above");
cleanup:
	if (ktimer_active(&timer)) {
		bool cancelled = ktimer_cancel(&timer);

		(void)cancelled;
	}
	while (irq_nesting() > saved_nesting)
		irq_exit();
	while (cpu_preempt_count(current_cpu()) > saved_preempt_count)
		preempt_enable();
	task_set_trap_frame(current_task(), saved_tf);
	local_irq_restore(saved_flags);

	return __test_ret;
}

int test_task_context_matrix(void)
{
	struct task_struct *saved_task = current_task();
	spinlock_t lock = SPINLOCK_INIT;
	irq_flags_t saved_flags = local_irq_save();
	irq_flags_t lock_flags;
	int saved_preempt_count = preempt_count();
	uint32_t saved_nesting = irq_nesting();

	TEST_BEGIN("irq: task-context matrix");
	{
		TEST_ASSERT_NOT_NULL(saved_task);
		TEST_ASSERT_NE(saved_task, &idle_task);
		TEST_ASSERT_EQ(saved_nesting, (uint32_t)0);
		TEST_ASSERT_EQ(saved_preempt_count, 0);

		/* IRQ-off task context remains task context. */
		local_irq_disable();
		TEST_ASSERT(irqs_disabled());
		TEST_ASSERT(!in_irq());
		TEST_ASSERT_EQ(preempt_count(), 0);
		TEST_ASSERT(in_task_context());
		TEST_ASSERT(wait_context_can_sleep());

		/* Hard IRQ context is independent from the hardware IRQ bit. */
		local_irq_enable();
		irq_enter();
		TEST_ASSERT(!irqs_disabled());
		TEST_ASSERT(in_irq());
		TEST_ASSERT(!in_task_context());
		TEST_ASSERT(!wait_context_can_sleep());
		TEST_ASSERT_EQ(preempt_count(), 0);
		irq_exit();

		/* Explicit preemption disable does not stop being task context. */
		preempt_disable();
		TEST_ASSERT(!preemptible());
		TEST_ASSERT(!in_irq());
		TEST_ASSERT(in_task_context());
		TEST_ASSERT(!wait_context_can_sleep());
		preempt_enable();
		TEST_ASSERT_EQ(preempt_count(), 0);
		TEST_ASSERT(wait_context_can_sleep());

		spin_lock_init(&lock);
		spin_lock_irqsave(&lock, &lock_flags);
		TEST_ASSERT(spinlock_held());
		TEST_ASSERT(!wait_context_can_sleep());
		spin_unlock_irqrestore(&lock, lock_flags);

		set_current_task(NULL);
		TEST_ASSERT(!in_task_context());
		TEST_ASSERT(!wait_context_can_sleep());
		set_current_task(&idle_task);
		TEST_ASSERT(!in_task_context());
		TEST_ASSERT(!wait_context_can_sleep());
		set_current_task(saved_task);
		TEST_ASSERT(in_task_context());
		TEST_ASSERT(wait_context_can_sleep());
	}
	TEST_END("irq: task-context matrix");
	goto cleanup;
fail:
	TEST_FAIL("irq: task-context matrix", "see above");
cleanup:
	set_current_task(saved_task);
	while (irq_nesting() > saved_nesting)
		irq_exit();
	while (preempt_count() > saved_preempt_count)
		preempt_enable();
	local_irq_restore(saved_flags);

	return __test_ret;
}

int test_trap_user_exception_classification(void)
{
	struct trap_exception exception;
	struct trap_frame tf;
	static const struct {
		uintptr_t cause;
		int sig;
		int code;
		uintptr_t addr;
	} cases[] = {
		{EXC_INST_MISALIGNED, SIGBUS, BUS_ADRALN, 0x3003},
		{EXC_LOAD_MISALIGNED, SIGBUS, BUS_ADRALN, 0x3003},
		{EXC_STORE_MISALIGNED, SIGBUS, BUS_ADRALN, 0x3003},
		{EXC_INST_ACCESS, SIGSEGV, SEGV_ACCERR, 0x3003},
		{EXC_LOAD_ACCESS, SIGSEGV, SEGV_ACCERR, 0x3003},
		{EXC_STORE_ACCESS, SIGSEGV, SEGV_ACCERR, 0x3003},
		{EXC_INST_ILLEGAL, SIGILL, ILL_ILLOPC, 0x1000},
		{EXC_BREAKPOINT, SIGTRAP, TRAP_BRKPT, 0x1000},
		{EXC_ECALL_S, SIGILL, ILL_ILLTRP, 0x1000},
		{63, SIGILL, SI_KERNEL, 0x1000},
	};

	TEST_BEGIN("trap: user exception classification");
	{
		memset(&tf, 0, sizeof(tf));
		trap_setup_user_return(&tf, 0x1000, 0x2000);
		tf.stval = 0x3003;

		for (size_t index = 0; index < ARRLEN(cases); index++) {
			tf.scause = cases[index].cause;
			exception = trap_classify_exception(&tf);
			TEST_ASSERT_EQ(exception.disposition,
				       TRAP_EXCEPTION_USER_SIGNAL);
			TEST_ASSERT_EQ(exception.info.si_signo,
				       cases[index].sig);
			TEST_ASSERT_EQ(exception.info.si_code,
				       cases[index].code);
			TEST_ASSERT_EQ((uintptr_t)exception.info.si_addr,
				       cases[index].addr);
		}

		tf.scause = EXC_LOAD_PAGE_FAULT;
		exception = trap_classify_exception(&tf);
		TEST_ASSERT_EQ(exception.disposition,
			       TRAP_EXCEPTION_PAGE_FAULT);

		trap_set_kernel_return(&tf, 0x1000);
		tf.scause = EXC_INST_ILLEGAL;
		exception = trap_classify_exception(&tf);
		TEST_ASSERT_EQ(exception.disposition,
			       TRAP_EXCEPTION_KERNEL_FATAL);
	}
	TEST_END("trap: user exception classification");
	return __test_ret;
fail:
	TEST_FAIL("trap: user exception classification", "see above");
	return __test_ret;
}

int test_signal_riscv_frame_abi(void)
{
	struct trap_frame tf;

	TEST_BEGIN("signal: riscv rt frame ABI");
	{
		TEST_ASSERT_EQ(sizeof(siginfo_t), (size_t)128);
		TEST_ASSERT_EQ(sizeof(struct user_regs_struct), (size_t)256);
		TEST_ASSERT_EQ(offsetof(struct user_regs_struct, a0),
			       (size_t)(10 * sizeof(unsigned long)));
		TEST_ASSERT_EQ(offsetof(struct ucontext, uc_mcontext),
			       (size_t)176);
		TEST_ASSERT_EQ(sizeof(struct rt_sigframe), (size_t)1088);

		memset(&tf, 0, sizeof(tf));
		trap_setup_signal_handler(&tf, 0x1000, 0x2000, 0x3000, 4,
					  0x4000, 0x5000);
		TEST_ASSERT_EQ(tf.sepc, 0x1000UL);
		TEST_ASSERT_EQ(tf.ra, 0x2000UL);
		TEST_ASSERT_EQ(tf.sp, 0x3000UL);
		TEST_ASSERT_EQ(tf.a0, 4UL);
		TEST_ASSERT_EQ(tf.a1, 0x4000UL);
		TEST_ASSERT_EQ(tf.a2, 0x5000UL);
	}
	TEST_END("signal: riscv rt frame ABI");
	return __test_ret;
fail:
	TEST_FAIL("signal: riscv rt frame ABI", "see above");
	return __test_ret;
}
