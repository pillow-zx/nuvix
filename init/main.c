/*
 * init/main.c - kernel_main() 内核初始化入口
 */

#include <kernel/printk.h>
#include <kernel/buddy.h>
#include <kernel/init.h>
#include <kernel/slab.h>
#include <kernel/page_cache.h>
#include <kernel/task.h>
#include <kernel/sched.h>
#include <kernel/timer.h>
#include <kernel/syscall.h>
#include <kernel/signal.h>
#include <kernel/user_map.h>
#include <kernel/vmalloc.h>
#include <kernel/vfs.h>
#include <drivers/virtio_blk.h>
#include <kernel/trap.h>
#include <kernel/processor.h>
#include <kernel/pgtable.h>
#include <kernel/exit.h>
#include <kernel/irq.h>
#include <kernel/user_map_arch.h>
#include <kernel/tty.h>
#include <kernel/smp.h>

static const char logo[] =
	"  /$$$$$$              /$$                /$$$$$$   /$$$$$$ \n"
	" /$$__  $$            | $$               /$$__  $$ /$$__  $$\n"
	"| $$  \\__/ /$$   /$$ /$$$$$$    /$$$$$$ | $$  \\ $$| $$  \\__/\n"
	"| $$      | $$  | $$|_  $$_/   /$$__  $$| $$  | $$|  $$$$$$ \n"
	"| $$      | $$  | $$  | $$    | $$$$$$$$| $$  | $$ \\____  $$\n"
	"| $$    $$| $$  | $$  | $$ /$$| $$_____/| $$  | $$ /$$  \\ $$\n"
	"|  $$$$$$/|  $$$$$$/  |  $$$$/|  $$$$$$$|  $$$$$$/|  $$$$$$/\n"
	" \\______/  \\______/    \\___/   \\_______/ \\______/  \\______/\n";

void kernel_main(uint64_t boot_hartid)
{
	struct task_struct *init;
	struct task_struct *writeback;
	int ret;

	console_init_sbi();

	pr_info("\n");
	pr_info("%s", logo);
	pr_info("\n");

	pagetable_init();
	console_init_mmio();
	tty_console_init();
	pr_info("uart: init successfully\n");

	buddy_init();
	pagetable_use_buddy();
	slab_init();
	vmalloc_init();
	user_map_init();
	BUG_ON(user_map_reserve("stack_guard", USER_STACK_GUARD_BASE,
				USER_STACK_BASE) < 0);
	signal_user_map_init();
	pr_info("mm: init successfully\n");

	/* Platform enumeration validates the boot hart and fills the
	 * topology; every configured hart is required to start later. */
	BUG_ON(smp_prepare((uint32_t)boot_hartid) < 0);

	/* Global initialization: every static queue and slot is reset once. */
	task_init();
	pr_info("task: init successfully\n");

	sched_init();
	pr_info("sched: init successfully\n");

	clockevent_init();
	pr_info("timer: init successfully\n");

	/* CPU 0-local initialization: touches only this hart's CSRs and slot.
	 * The Sstc timer is programmed only after task_init, so a timer IRQ
	 * can never fire with no current task installed. */
	trap_cpu_init();
	trap_cpu_init_print();
	pr_info("trap: init successfully\n");

	timer_cpu_init();
	clockevent_cpu_init();

	/* Publish CPU 0 only after its idle/current and local state exist. */
	cpu_state_store_release(&cpu_table[0], CPU_ONLINE);
	cpu_set_online(0);
	cpu_set_schedulable(0);

	/* Boot every configured secondary into its isolated idle loop
	 * before any syscall/VFS/device/thread initialization. */
	smp_boot_cpus();

	syscall_init();
	pr_info("syscall: init successfully\n");

	vfs_init();
	pr_info("vfs: init successfully\n");

	ret = filesystems_init();
	if (ret < 0)
		panic("filesystems: init failed (%d)", ret);
	pr_info("filesystems: init successfully\n");

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
