/*
 * kernel/init_process.c - PID 1 init 内核线程
 */

#include <nuvix/exec.h>
#include <nuvix/bootinfo.h>
#include <nuvix/init.h>
#include <nuvix/printk.h>
#include <nuvix/task.h>

void init_process(void *arg)
{
	(void)arg;
	if (task_create_initial_proc(current_task()) < 0)
		panic("init: failed to create process object");
	bootinfo_init();

	exec_user_path("/sbin/init");
}

BOOTINFO_BLOCK(init, void,
	BBLANK();
	BROW("Init Process", "running (PID %u)",
	     (unsigned)current_task()->proc->pid->nr);
	BROW("Boot Complete", "handoff to user space");
	BBLANK();
)

bool init_process_is_task(const struct task_struct *task)
{
	return task && init_task && task->proc && init_task->proc &&
		task->proc->pid && init_task->proc->pid &&
		task->proc->pid->nr == init_task->proc->pid->nr;
}
