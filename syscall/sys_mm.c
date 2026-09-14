/*
 * syscall/sys_mm.c - 内存相关系统调用
 */

#include <nuvix/errno.h>
#include <nuvix/syscall.h>
#include <nuvix/mm.h>
#include <nuvix/page.h>
#include <nuvix/task.h>
#include <uapi/mman.h>
#include <nuvix/trap.h>

ssize_t sys_brk(struct trap_frame *tf)
{
	uintptr_t addr = (uintptr_t)syscall_arg(tf, 0);

	return (ssize_t)mm_brk(current_task()->proc ? current_task()->proc->mm : NULL,
			       addr);
}

/* Anonymous private/shared and regular-file private mappings only. */
ssize_t sys_mmap(struct trap_frame *tf)
{
	uintptr_t addr = (uintptr_t)syscall_arg(tf, 0);
	size_t length = (size_t)syscall_arg(tf, 1);
	int prot = (int)syscall_arg(tf, 2);
	int flags = (int)syscall_arg(tf, 3);
	int fd = (int)syscall_arg(tf, 4);
	uint64_t offset = (uint64_t)syscall_arg(tf, 5);

	return mm_mmap_file(current_task()->proc ? current_task()->proc->mm : NULL,
			    addr, length, prot, flags,
			    fd, offset);
}

ssize_t sys_munmap(struct trap_frame *tf)
{
	uintptr_t addr = (uintptr_t)syscall_arg(tf, 0);
	size_t length = (size_t)syscall_arg(tf, 1);

	return mm_munmap(current_task()->proc ? current_task()->proc->mm : NULL,
			 addr, length);
}

/*
 * SYSCALL_SUPPORT(B): mprotect
 * Current: updates VMA ranges and resident PTE permissions.
 * Unsupported errno: unaligned address or invalid prot returns -EINVAL;
 * unmapped ranges return -ENOMEM.
 */
ssize_t sys_mprotect(struct trap_frame *tf)
{
	uintptr_t addr = (uintptr_t)syscall_arg(tf, 0);
	size_t length = (size_t)syscall_arg(tf, 1);
	int prot = (int)syscall_arg(tf, 2);

	return mm_mprotect(current_task()->proc ? current_task()->proc->mm : NULL,
			   addr, length, prot);
}
