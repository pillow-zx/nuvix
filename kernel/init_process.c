/*
 * kernel/init_process.c - PID 1 init 内核线程
 */

#include <nuvix/exec.h>
#include <nuvix/init.h>
#include <nuvix/printk.h>
#include <nuvix/task.h>
#include <nuvix/proc.h>
#include <nuvix/bootdev.h>
#include <nuvix/page_cache.h>
#include <nuvix/vfs.h>
#include <nuvix/tty.h>
#include <drivers/virtio_blk.h>

void init_process(void *arg)
{
	dev_t bootdev;
	(void)arg;
	/* This remains the first non-idle task (PID 1). Spawn device workers
	 * here, after its identity and the scheduler have been established. */
	int ret = boot_console_start();
	if (ret < 0)
		panic("console: IRQ start failed (%d)", ret);
	ret = printk_console_start();
	if (ret < 0)
		panic("console: log thread init failed (%d)", ret);
	ret = tty_console_start();
	if (ret < 0)
		panic("console: input thread init failed (%d)", ret);
	/* Root probing performs disk I/O and must be allowed to sleep. */
	bootdev = boot_disk_init();
	if (!kernel_thread(virtio_blk_watchdog_thread, NULL))
		panic("init: block watchdog init failed");
	ret = vfs_mount_root(bootdev);
	if (ret < 0)
		panic("VFS: root mount failed (%d)", ret);
	/* Publish the cache through root mounting before writeback can run. */
	if (!kernel_thread(pgcache_wb_thread, NULL))
		panic("init: writeback thread init failed");
	if (!kernel_thread(pgcache_async_thread, NULL))
		panic("init: cache completion thread init failed");
	if (task_create_initial_proc(current_task()) < 0)
		panic("init: failed to create process object");
	exec_user_path("/sbin/init");
}

bool init_process_is_task(const struct task_struct *task)
{
	return task && init_task && task->proc && init_task->proc &&
		task->proc->pid && init_task->proc->pid &&
		task->proc->pid->nr == init_task->proc->pid->nr;
}
