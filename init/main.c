/*
 * init/main.c - kernel_main() 内核初始化入口
 */

#include <nuvix/printk.h>
#include <nuvix/bootinfo.h>
#include <nuvix/buddy.h>
#include <nuvix/init.h>
#include <nuvix/slab.h>
#include <nuvix/page_cache.h>
#include <nuvix/task.h>
#include <nuvix/sched.h>
#include <nuvix/timer.h>
#include <nuvix/syscall.h>
#include <nuvix/signal.h>
#include <nuvix/vmalloc.h>
#include <nuvix/vfs.h>
#include <nuvix/bootdev.h>
#include <arch/boot.h>
#include <nuvix/trap.h>
#include <nuvix/processor.h>
#include <nuvix/pgtable.h>
#include <nuvix/exit.h>
#include <nuvix/irq.h>
#include <nuvix/tty.h>
#include <nuvix/smp.h>
#include <nuvix/cpu.h>

void kernel_main(uint64_t hartid, paddr_t dtb_pa)
{
	struct task_struct *init;
	struct task_struct *writeback;
	int ret;

	console_init_sbi();
	platform_init(hartid, dtb_pa);

	/* Hardware facts have been validated before any memory/device setup. */
	bootinfo_logo();
	bootinfo_platform(hartid);
	bootinfo_sbi();
	bootinfo_timer();

	pgtable_init();
	buddy_init();
	boot_devices_prepare();
	tty_console_init();
	slab_init();
	vmalloc_init();
	sig_init();
	bootinfo_mm();

	/* Publish the DT topology; every selected hart must start later. */
	BUG_ON(cpu_prepare((uint32_t)hartid) < 0);
	smp_prepare();

	/* Global initialization: every static queue and slot is reset once. */
	task_init();

	sched_init();

	clockevent_init();

	/* Logical CPU 0-local initialization: touches only this hart's CSRs and
	 * slot.
	 * The timer is programmed only after task_init, so a timer IRQ
	 * can never fire with no current task installed. */
	trap_cpu_init();

	timer_cpu_init();
	clockevent_cpu_init();

	/* The boot CPU is ready in both UP and SMP builds. Start secondaries
	 * before any syscall/VFS/device/thread initialization. */
	cpu_boot_online();
	smp_boot_cpus();
	/* Local trap/timer state and the current idle task are ready, and
	 * all online CPUs can service IPIs. Filesystem initialization below
	 * uses vmalloc, whose global shootdowns require IRQs enabled here. */
	local_irq_enable();
	/* Close the banner with the CPU block: online/schedulable masks are
	 * final once every secondary has published ONLINE. */
	bootinfo_cpu();

	syscall_init();

	vfs_init();

	ret = filesystems_init();
	if (ret < 0)
		panic("filesystems: init failed (%d)", ret);

	ret = vfs_mount_root(boot_disk_init());
	if (ret < 0)
		panic("VFS: root mount failed (%d)", ret);

	init = kernel_thread(init_process, NULL);
	BUG_ON(!init);
	set_init_task(init);
	if (task_reaper_start() < 0)
		panic("task: reaper init failed");

	ret = tty_console_start();
	if (ret < 0)
		panic("console: input thread init failed (%d)", ret);

	writeback = kernel_thread(pgcache_wb_thread, NULL);
	BUG_ON(!writeback);

	while (true) {
		local_irq_enable();
		schedule();
		sched_idle();
	}
}
