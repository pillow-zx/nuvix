/*
 * mm/uaccess.c - 用户空间内存访问
 */

#include <kernel/mm.h>
#include <kernel/errno.h>
#include <kernel/task.h>
#include <kernel/page.h>
#include <kernel/uaccess_arch.h>

#include "internal.h"

bool access_ok(const void *addr, size_t size)
{
	vaddr_t a = (vaddr_t)addr;

	if (size == 0)
		return true;

	if (a + size < a)
		return false;

	if (a + size > TASK_SIZE)
		return false;

	return true;
}

int user_range_probe(const void *addr, size_t size, bool write)
{
	struct task_struct *task = current_task();
	struct mm_struct *mm;
	int access;

	if (size == 0)
		return 0;
	if (!access_ok(addr, size))
		return -EFAULT;
	mm = task->proc ? task->proc->mm : NULL;
	if (!mm)
		return -EFAULT;

	access = write ? USER_FAULT_WRITE : USER_FAULT_READ;
	return fault_in_user_range(mm, (uintptr_t)addr, size, access);
}

size_t copy_to_user(void *to, const void *from, size_t n)
{
	if (user_range_probe(to, n, true) < 0)
		return n;

	bool had_sum = user_access_begin();
	memcpy(to, from, n);
	user_access_end(had_sum);

	return 0;
}

size_t copy_from_user(void *to, const void *from, size_t n)
{
	if (user_range_probe(from, n, false) < 0)
		return n;

	bool had_sum = user_access_begin();
	memcpy(to, from, n);
	user_access_end(had_sum);

	return 0;
}

ssize_t strncpy_from_user(char *dst, const char *src, size_t maxlen)
{
	size_t done = 0;

	if (!dst)
		return -EINVAL;
	if (!src)
		return -EFAULT;
	if (maxlen == 0)
		return -ENAMETOOLONG;

	while (done < maxlen) {
		uintptr_t addr = (uintptr_t)src + done;
		struct vm_area_struct *vma;
		size_t chunk = MIN(maxlen - done,
				   PAGE_SIZE - (addr & (PAGE_SIZE - 1)));
		struct mm_struct *mm;

		if (!access_ok((const void *)addr, 1))
			return -EFAULT;
		mm = current_task()->proc ? current_task()->proc->mm : NULL;
		if (!mm)
			return -EFAULT;
		with_guard(mm_guard, mm)
		{
			vma = find_vma(mm, addr);
			if (!vma || !(vma->vm_flags & VM_READ))
				return -EFAULT;

			chunk = MIN(chunk, vma->vm_end - addr);
		}
		if (chunk == 0)
			return -EFAULT;

		if (user_range_probe((const void *)addr, chunk, false) < 0)
			return -EFAULT;

		bool had_sum = user_access_begin();
		for (size_t i = 0; i < chunk; i++) {
			char c = ((const char *)addr)[i];

			dst[done + i] = c;
			if (c == '\0') {
				user_access_end(had_sum);
				return (ssize_t)(done + i);
			}
		}
		user_access_end(had_sum);
		done += chunk;
	}

	dst[maxlen - 1] = '\0';
	return -ENAMETOOLONG;
}
