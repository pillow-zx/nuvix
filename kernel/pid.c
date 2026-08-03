/*
 * kernel/pid.c - PID 分配器
 */

#include <kernel/pid.h>
#include <kernel/bitmap.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/mutex.h>

#include "pid_internal.h"
#include "task_internal.h"

BITMAP_DECLARE_STATIC(pid_map, PID_COUNT);
static struct task_struct *pid_tasks[PID_COUNT];
static DEFINE_MUTEX(pid_lock);

void pid_init(void)
{
	bitmap_zero(&pid_map);
	memset(pid_tasks, 0, sizeof(pid_tasks));

	bitmap_set(&pid_map, 0);

	pr_info("pid: bitmap initialized (%zu PIDs, 0 reserved for idle)\n",
		PID_COUNT);
}

int32_t alloc_pid(void)
{
	size_t pid;
	int32_t ret;

	mutex_lock(&pid_lock);
	pid = bitmap_find_first_zero(&pid_map);
	if (pid >= PID_COUNT) {
		ret = -ENOSPC;
	} else {
		bitmap_set(&pid_map, pid);
		ret = (int32_t)pid;
	}
	mutex_unlock(&pid_lock);

	return ret;
}

void free_pid(pid_t pid)
{
	if (pid == 0) {
		pr_warn("pid: cannot free PID 0 (idle)\n");
		return;
	}

	if (pid < 0 || pid > PID_MAX)
		return;

	mutex_lock(&pid_lock);
	BUG_ON(pid_tasks[pid]);
	bitmap_clear(&pid_map, pid);
	mutex_unlock(&pid_lock);
}

void pid_attach_task(pid_t pid, struct task_struct *task)
{
	BUG_ON(pid < 0 || pid > PID_MAX);
	BUG_ON(!task);
	mutex_lock(&pid_lock);
	BUG_ON(pid_tasks[pid] && pid_tasks[pid] != task);
	BUG_ON(task->lifecycle.published);

	task->lifecycle.published = true;
	pid_tasks[pid] = task;
	mutex_unlock(&pid_lock);
}

void pid_detach_task(pid_t pid, struct task_struct *task)
{
	if (pid < 0 || pid > PID_MAX)
		return;
	mutex_lock(&pid_lock);
	if (pid_tasks[pid] == task) {
		pid_tasks[pid] = NULL;
		task->lifecycle.published = false;
	}
	mutex_unlock(&pid_lock);
}

struct task_struct *pid_task_get(pid_t pid)
{
	struct task_struct *task = NULL;

	if (pid < 0 || pid > PID_MAX)
		return NULL;

	mutex_lock(&pid_lock);
	if (pid_tasks[pid] && pid_tasks[pid]->lifecycle.published &&
	    task_try_get(pid_tasks[pid]))
		task = pid_tasks[pid];
	mutex_unlock(&pid_lock);

	return task;
}

bool pid_visit_published(pid_task_visit_fn visit, void *arg)
{
	bool found = false;

	if (!visit)
		return false;

	/* The registry lock keeps each visited task published and alive. */
	mutex_lock(&pid_lock);
	for (pid_t pid = 1; pid <= PID_MAX; pid++) {
		struct task_struct *task = pid_tasks[pid];

		if (task && visit(task, arg)) {
			found = true;
			break;
		}
	}
	mutex_unlock(&pid_lock);
	return found;
}

uint16_t pid_count_tasks(void)
{
	uint16_t count = 0;

	mutex_lock(&pid_lock);
	for (pid_t pid = 1; pid <= PID_MAX; pid++) {
		if (pid_tasks[pid])
			count++;
	}
	mutex_unlock(&pid_lock);

	return count;
}
