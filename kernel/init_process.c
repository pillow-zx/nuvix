/*
 * kernel/init_process.c - PID 1 init 内核线程
 */

#include <kernel/exec.h>
#include <kernel/init.h>
#include <kernel/printk.h>
#include <kernel/task.h>

void init_process(void *arg)
{
	(void)arg;
	if (task_create_initial_proc(current_task()) < 0)
		panic("init: failed to create process object");
	pr_info("init running (PID %d)\n", current_task()->proc->pid->nr);

	exec_user_path("/sbin/init");
}

bool init_process_is_task(const struct task_struct *task)
{
	return task && init_task && task->proc && init_task->proc &&
		task->proc->pid && init_task->proc->pid &&
		task->proc->pid->nr == init_task->proc->pid->nr;
}
