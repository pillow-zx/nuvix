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
#include <kernel/reboot.h>
#include <kernel/exit.h>
#include <kernel/irq.h>
#include <kernel/user_map_arch.h>
#include <kernel/tty.h>

static const char logo[] =
	"  /$$$$$$              /$$                /$$$$$$   /$$$$$$ \n"
	" /$$__  $$            | $$               /$$__  $$ /$$__  $$\n"
	"| $$  \\__/ /$$   /$$ /$$$$$$    /$$$$$$ | $$  \\ $$| $$  \\__/\n"
	"| $$      | $$  | $$|_  $$_/   /$$__  $$| $$  | $$|  $$$$$$ \n"
	"| $$      | $$  | $$  | $$    | $$$$$$$$| $$  | $$ \\____  $$\n"
	"| $$    $$| $$  | $$  | $$ /$$| $$_____/| $$  | $$ /$$  \\ $$\n"
	"|  $$$$$$/|  $$$$$$/  |  $$$$/|  $$$$$$$|  $$$$$$/|  $$$$$$/\n"
	" \\______/  \\______/    \\___/   \\_______/ \\______/  \\______/\n";

#if defined(KERNEL_SELFTEST) || defined(KERNEL_PANIC_TEST)
#include <kernel/test.h>
#endif

#ifdef KERNEL_SELFTEST
static void kernel_selftest_thread(void *arg)
{
	struct ktest_summary test_summary;
	int ret;

	(void)arg;

	ret = kernel_test_run(&test_summary);
	pr_info("[KTEST] done modules=%u failed_modules=%u cases=%u "
		"failed_cases=%u\n",
		test_summary.modules, test_summary.failed_modules,
		test_summary.cases, test_summary.failed_cases);
	if (ret < 0)
		pr_err("[KTEST] result failed\n");
	else
		pr_info("[KTEST] result passed\n");
	ret = kernel_reboot(KERNEL_REBOOT_POWER_OFF);
	BUG_ON(ret < 0);
}
#endif

void kernel_main(void)
{
#ifdef KERNEL_SELFTEST
	struct task_struct *selftest;
#else
	struct task_struct *init;
	struct task_struct *writeback;
	int ret;
#endif

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

	trap_init();
	pr_info("trap: init successfully\n");

	task_init();
	pr_info("task: init successfully\n");

#ifdef KERNEL_PANIC_TEST
	kernel_panic_test_run();
#endif

	timer_init();
	pr_info("timer: init successfully\n");

	sched_init();
	pr_info("sched: init successfully\n");

	syscall_init();
	pr_info("syscall: init successfully\n");

	vfs_init();
	pr_info("vfs: init successfully\n");

#ifdef KERNEL_SELFTEST
	/* KTEST exercises kernel mechanisms with in-memory fixtures. */
	pr_info("storage: skipped for kernel self-test\n");
#else
	ret = filesystems_init();
	if (ret < 0)
		panic("filesystems: init failed (%d)", ret);
	pr_info("filesystems: init successfully\n");

	virtio_blk_init();
	ret = vfs_mount_root(ROOT_DEV);
	if (ret < 0)
		panic("VFS: root mount failed (%d)", ret);
#endif

#ifdef KERNEL_SELFTEST
	selftest = kernel_thread(kernel_selftest_thread, NULL);
	BUG_ON(!selftest);
#else
	init = kernel_thread(init_process, NULL);
	BUG_ON(!init);
	set_init_task(init);

	ret = tty_console_start();
	if (ret < 0)
		panic("console: input thread init failed (%d)", ret);

	writeback = kernel_thread(page_cache_wb_thread, NULL);
	BUG_ON(!writeback);
#endif

	while (true) {
		local_irq_enable();
		reap_exited_threads();
		schedule();
		wait_for_interrupt();
	}
}
