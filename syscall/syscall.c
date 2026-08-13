/*
 * syscall/syscall.c - 系统调用分发
 */

#include <kernel/syscall.h>
#include <kernel/errno.h>
#include <kernel/futex.h>
#include <kernel/printk.h>
#include <kernel/signal.h>
#include <kernel/syscall_table.h>
#include <kernel/task.h>
#include <kernel/trap.h>
#include <uapi/futex.h>
#include <uapi/syscall.h>

typedef ssize_t (*syscall_fn_t)(struct trap_frame *);

static syscall_fn_t syscall_table[NR_SYSCALL];

void do_syscall(struct trap_frame *tf)
{
	size_t nr = syscall_nr(tf);
	ssize_t ret;
	struct task_struct *task = current_task();

	if (task)
		restart_save(task, tf, nr);

	if (nr >= NR_SYSCALL || !syscall_table[nr]) {
		ret = -ENOSYS;
		if (task)
			restart_finish(task, ret);
		syscall_set_return(tf, ret);
		return;
	}

	ret = syscall_table[nr](tf);
	if (task)
		restart_finish(task, ret);
	syscall_set_return(tf, ret);
}

void syscall_init(void)
{
	futex_init();

#define INSTALL_SYSCALL(nr, name, fn) syscall_table[nr] = fn;
	SYSCALL_TABLE(INSTALL_SYSCALL)
#undef INSTALL_SYSCALL

	pr_info("syscall: initialized (%d entries)\n", NR_SYSCALL);
}
