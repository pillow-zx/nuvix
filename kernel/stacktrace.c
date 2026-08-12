#include <kernel/ksyms.h>
#include <kernel/printk.h>
#include <kernel/stacktrace.h>
#include <kernel/task.h>
#include <kernel/types.h>

#define STACKTRACE_MAX_DEPTH 16

static void current_stack_bounds(uintptr_t *low, uintptr_t *high)
{
	struct task_struct *task = current_task();

	if (!task)
		task = cpu_idle_task(current_cpu());
	if (!task || !task->arch.kstack) {
		*low = 0;
		*high = 0;
		return;
	}
	*low = (uintptr_t)task->arch.kstack;
	*high = *low + KSTACK_SIZE;
}

static bool frame_pointer_valid(uintptr_t fp, uintptr_t low, uintptr_t high)
{
	return (fp & (sizeof(uintptr_t) - 1)) == 0 &&
	       fp >= low + 2 * sizeof(uintptr_t) && fp <= high;
}

void dump_stack(void)
{
	uintptr_t low;
	uintptr_t high;
	uintptr_t fp = (uintptr_t)__frame_address();

	current_stack_bounds(&low, &high);
	pr_err("Call Trace:\n");

	for (uint32_t depth = 0; depth < STACKTRACE_MAX_DEPTH; depth++) {
		uintptr_t ra;
		uintptr_t next_fp;
		uintptr_t offset = 0;
		const char *name;

		if (!frame_pointer_valid(fp, low, high))
			break;

		ra = *(uintptr_t *)(fp - sizeof(uintptr_t));
		next_fp = *(uintptr_t *)(fp - 2 * sizeof(uintptr_t));
		if (!ra)
			break;

		name = ksym_lookup(ra, &offset);
		if (name)
			pr_err("  [%d] %p <%s+0x%lx>\n", (int)depth, (void *)ra,
			       name, (size_t)offset);
		else
			pr_err("  [%d] %p\n", (int)depth, (void *)ra);

		if (next_fp <= fp)
			break;
		fp = next_fp;
	}
}
