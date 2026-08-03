

#include <kernel/test.h>
#include <kernel/printk.h>
#include <kernel/buddy.h>
#include <kernel/irq.h>
#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/page.h>
#include <kernel/pgtable.h>
#include <kernel/trap.h>
#include <kernel/processor.h>
#include <kernel/mm.h>
#include <kernel/vmalloc.h>
#include <kernel/signal.h>
#include <uapi/mman.h>

#include "../ktest.h"

extern void __trapret(void);
extern char user_trap_test_start[];
extern char user_trap_test_illegal[];
extern char user_trap_test_handler[];
extern char user_trap_test_ecall[];
extern char user_trap_test_end[];

#define UTEST_RA  0x0101010101010101UL
#define UTEST_T0  0x0202020202020202UL
#define UTEST_T1  0x0303030303030303UL
#define UTEST_T2  0x0404040404040404UL
#define UTEST_S0  0x0505050505050505UL
#define UTEST_S1  0x0606060606060606UL
#define UTEST_A0  0x0707070707070707UL
#define UTEST_A1  0x0808080808080808UL
#define UTEST_A2  0x0909090909090909UL
#define UTEST_A3  0x0A0A0A0A0A0A0A0AUL
#define UTEST_A4  0x0B0B0B0B0B0B0B0BUL
#define UTEST_A5  0x0C0C0C0C0C0C0C0CUL
#define UTEST_A6  0x0D0D0D0D0D0D0D0DUL
#define UTEST_A7  0x0E0E0E0E0E0E0E0EUL
#define UTEST_S2  0x0F0F0F0F0F0F0F0FUL
#define UTEST_S3  0x1010101010101010UL
#define UTEST_S4  0x1111111111111111UL
#define UTEST_S5  0x1212121212121212UL
#define UTEST_S6  0x1313131313131313UL
#define UTEST_S7  0x1414141414141414UL
#define UTEST_S8  0x1515151515151515UL
#define UTEST_S9  0x1616161616161616UL
#define UTEST_S10 0x1717171717171717UL
#define UTEST_S11 0x1818181818181818UL
#define UTEST_T3  0x1919191919191919UL
#define UTEST_T4  0x1A1A1A1A1A1A1A1AUL
#define UTEST_T5  0x1B1B1B1B1B1B1B1BUL
#define UTEST_T6  0x1C1C1C1C1C1C1C1CUL

static volatile bool user_trap_test_trapped;
static volatile bool user_trap_test_resumed;
static volatile const char *user_trap_test_fail_msg;
static struct task_struct *user_trap_test_task;
static struct task_struct *user_trap_test_runner_task;
static uintptr_t user_trap_test_user_sp;
static uintptr_t user_trap_test_ecall_va;

__noreturn
static void user_trap_test_resume(void);

static bool user_trap_test_hook(struct trap_frame *tf)
{
#define USER_TRAP_CHECK(cond, msg)                                             \
	do {                                                                   \
		if (!(cond)) {                                                 \
			user_trap_test_fail_msg = (msg);                       \
			goto intercept;                                        \
		}                                                              \
	} while (0)

	if (current_task() != user_trap_test_task)
		return false;
	if (trap_frame_cause(tf) != EXC_ECALL_U)
		return false;
	if (trap_frame_cause(tf) == EXC_ECALL_U &&
	    trap_user_pc(tf) == SIGNAL_TRAMPOLINE_ADDR + sizeof(uint32_t))
		return false;

	user_trap_test_trapped = true;

	USER_TRAP_CHECK(trap_frame_from_user(tf),
			"trap did not originate from user mode");
	USER_TRAP_CHECK(trap_frame_cause(tf) == EXC_ECALL_U,
			"unexpected trap cause while running user test");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_STVAL) == 0,
			"ecall stval should be zero");
	USER_TRAP_CHECK(trap_user_pc(tf) == user_trap_test_ecall_va,
			"saved sepc does not point to user ecall");
	USER_TRAP_CHECK(trap_user_sp(tf) == user_trap_test_user_sp,
			"user sp was not restored correctly");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_RA) == UTEST_RA,
			"ra was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_T0) == UTEST_T0,
			"t0 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_T1) == UTEST_T1,
			"t1 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_T2) == UTEST_T2,
			"t2 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_S0) == UTEST_S0,
			"s0 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_S1) == UTEST_S1,
			"s1 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_A0) == UTEST_A0,
			"a0 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_A1) == UTEST_A1,
			"a1 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_A2) == UTEST_A2,
			"a2 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_A3) == UTEST_A3,
			"a3 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_A4) == UTEST_A4,
			"a4 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_A5) == UTEST_A5,
			"a5 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_A6) == UTEST_A6,
			"a6 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_A7) == UTEST_A7,
			"a7 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_S2) == UTEST_S2,
			"s2 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_S3) == UTEST_S3,
			"s3 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_S4) == UTEST_S4,
			"s4 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_S5) == UTEST_S5,
			"s5 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_S6) == UTEST_S6,
			"s6 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_S7) == UTEST_S7,
			"s7 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_S8) == UTEST_S8,
			"s8 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_S9) == UTEST_S9,
			"s9 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_S10) == UTEST_S10,
			"s10 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_S11) == UTEST_S11,
			"s11 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_T3) == UTEST_T3,
			"t3 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_T4) == UTEST_T4,
			"t4 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_T5) == UTEST_T5,
			"t5 was not preserved");
	USER_TRAP_CHECK(trap_test_reg(tf, ARCH_TRAP_REG_T6) == UTEST_T6,
			"t6 was not preserved");

intercept:

	trap_set_kernel_return(tf, (uintptr_t)user_trap_test_resume);
	return true;

#undef USER_TRAP_CHECK
}

__noreturn
static void user_trap_test_resume(void)
{
	struct task_struct *prev = current_task();
	struct task_struct *next = user_trap_test_runner_task;

	local_irq_disable();
	user_trap_test_resumed = true;
	prev->lifecycle.state = TASK_DEAD;
	BUG_ON(!next);
	if (task_is_queued(next))
		sched_dequeue(next);
	set_current_task(next);
	task_switch(prev, next);
	panic("user trap test resume returned unexpectedly");
}

static struct task_struct *
forge_user_return_task(size_t user_pc, size_t user_sp, size_t user_sstatus)
{
	struct task_struct *task = task_alloc();
	if (!task)
		return NULL;

	arch_task_test_setup_user_return(task, user_pc, user_sp, user_sstatus);

	return task;
}

int test_trap_user_return_task_setup(void)
{
	struct task_struct *t = NULL;
	struct mm_struct *mm = NULL;
	void *code_page = NULL;
	void *stack_page = NULL;
	bool enqueued = false;
	bool hook_installed = false;
	bool timer_quiesced = false;
	unsigned long saved_sie = 0;
	uintptr_t user_pc = 0;
	uintptr_t user_sp = 0;
	const uintptr_t code_va = 0x400000UL;
	size_t stub_size = 0;

	TEST_BEGIN("trap: runtime user entry and return");
	{
		TEST_ASSERT(sched_test_runqueue_empty());

		code_page = get_free_page(0, ALLOC_NOWAIT);
		TEST_ASSERT_NOT_NULL(code_page);
		stack_page = vmalloc(USER_STACK_SIZE, ALLOC_NOWAIT);
		TEST_ASSERT_NOT_NULL(stack_page);

		memset(stack_page, 0, USER_STACK_SIZE);

		stub_size = (size_t)(user_trap_test_end - user_trap_test_start);
		TEST_ASSERT(stub_size > 0);
		TEST_ASSERT(stub_size <= PAGE_SIZE);
		memcpy(code_page, user_trap_test_start, stub_size);
		mm = mm_create_user();
		TEST_ASSERT_NOT_NULL(mm);
		TEST_ASSERT_EQ(mm_map_segment(mm, code_va, code_va + PAGE_SIZE,
					      PROT_READ | PROT_EXEC),
			       0);
		TEST_ASSERT_EQ(mm_map_page(mm, code_va, code_page,
					   PROT_READ | PROT_EXEC),
			       0);
		TEST_ASSERT_EQ(mm_add_stack(mm, stack_page, USER_STACK_SIZE),
			       0);
		vfree(stack_page);
		stack_page = NULL;
		user_pc = code_va;
		user_sp = USER_STACK_TOP;

		t = forge_user_return_task(user_pc, user_sp, 0);
		TEST_ASSERT_NOT_NULL(t);
		TEST_ASSERT_EQ(task_init_resources(t), 0);
		task_set_mm(t, mm);
		task_set_satp(t, mm_user_satp(mm));
		struct task_struct *saved_task = current_task();
		set_current_task(t);
		struct sigaction action = {
			.sa_sigaction =
				(__sigactionhandler_t)(code_va +
						       (uintptr_t)(user_trap_test_handler -
								   user_trap_test_start)),
			.sa_flags = SA_SIGINFO,
		};
		TEST_ASSERT_EQ(do_sigaction(SIGILL, &action, NULL), 0);
		set_current_task(saved_task);

		user_trap_test_trapped = false;
		user_trap_test_resumed = false;
		user_trap_test_fail_msg = NULL;
		user_trap_test_task = t;
		user_trap_test_runner_task = current_task();
		user_trap_test_user_sp = user_sp;
		user_trap_test_ecall_va =
			user_pc + (uintptr_t)(user_trap_test_ecall -
					      user_trap_test_start);

		trap_set_hook(user_trap_test_hook);
		hook_installed = true;

		saved_sie = csr_read(sie);
		csr_clear(sie, SIE_STIE);
		timer_quiesced = true;

		sched_enqueue(t);
		enqueued = true;
		schedule();
		enqueued = false;

		csr_write(sie, saved_sie);
		timer_quiesced = false;

		trap_set_hook(NULL);
		hook_installed = false;

		TEST_ASSERT_EQ((size_t)current_task(),
			       (size_t)user_trap_test_runner_task);
		TEST_ASSERT(user_trap_test_trapped == true);
		TEST_ASSERT(user_trap_test_resumed == true);
		TEST_ASSERT_NULL((void *)user_trap_test_fail_msg);
		TEST_ASSERT(sched_test_runqueue_empty());
	}

	TEST_END("trap: runtime user entry and return");
	goto cleanup;
fail:
	TEST_FAIL("trap: runtime user entry and return", "see above");
cleanup:
	if (hook_installed)
		trap_set_hook(NULL);
	if (timer_quiesced) {
		csr_write(sie, saved_sie);
		timer_quiesced = false;
	}
	if (enqueued)
		sched_dequeue(t);
	if (t)
		task_set_mm(t, NULL);
	if (t)
		task_set_satp(t, 0);
	if (t)
		task_free(t);
	if (code_page && !mm)
		free_page(code_page, 0);
	if (stack_page && !mm)
		vfree(stack_page);
	return __test_ret;
}
