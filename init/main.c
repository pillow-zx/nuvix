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
#include <nuvix/user_map.h>
#include <nuvix/vmalloc.h>
#include <nuvix/vfs.h>
#include <drivers/virtio_blk.h>
#include <nuvix/trap.h>
#include <nuvix/processor.h>
#include <nuvix/pgtable.h>
#include <nuvix/exit.h>
#include <nuvix/irq.h>
#include <nuvix/user_map_arch.h>
#include <nuvix/tty.h>
#include <nuvix/smp.h>

void kernel_main(uint64_t hartid)
{
	struct task_struct *init;
	struct task_struct *writeback;
	int ret;

	console_init_sbi();

	/* OpenSBI-style banner, emitted incrementally as data becomes ready.
	 * Phase A needs only compile-time/link-time facts plus early SBI
	 * calls, so it prints right after the SBI console is up; later blocks
	 * follow at the points where their data exists. */
	bootinfo_logo();
	bootinfo_platform(hartid);
	bootinfo_sbi();
	bootinfo_timer();

	pagetable_init();
	console_init_mmio();
	tty_console_init();

	buddy_init();
	pagetable_use_buddy();
	slab_init();
	vmalloc_init();
	user_map_init();
	BUG_ON(user_map_reserve("stack_guard", USER_STACK_GUARD_BASE,
				USER_STACK_BASE) < 0);
	sig_init();
	bootinfo_mm();

	/* Platform enumeration validates the boot hart and fills the
	 * topology; every configured hart is required to start later. */
	BUG_ON(smp_prepare((uint32_t)hartid) < 0);

	/* Global initialization: every static queue and slot is reset once. */
	task_init();

	sched_init();

	clockevent_init();

	/* Logical CPU 0-local initialization: touches only this hart's CSRs and
	 * slot.
	 * The Sstc timer is programmed only after task_init, so a timer IRQ
	 * can never fire with no current task installed. */
	trap_cpu_init();

	timer_cpu_init();
	clockevent_cpu_init();

	/* Boot every configured secondary into its isolated idle loop
	 * before any syscall/VFS/device/thread initialization. The boot
	 * CPU's online/schedulable publication happens inside smp_boot_cpus().
	 */
	smp_boot_cpus();
	/* Close the banner with the CPU block: online/schedulable masks are
	 * final once every secondary has published ONLINE. */
	bootinfo_cpu();

	syscall_init();

	vfs_init();

	ret = filesystems_init();
	if (ret < 0)
		panic("filesystems: init failed (%d)", ret);

	virtio_blk_init();
	ret = vfs_mount_root(ROOT_DEV);
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
		local_irq_enable();
		wait_for_interrupt();
	}
}
