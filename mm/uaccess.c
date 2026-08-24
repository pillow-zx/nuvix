/*
 * mm/uaccess.c - 用户空间内存访问
 */

#include <nuvix/mm.h>
#include <nuvix/math.h>
#include <nuvix/errno.h>
#include <nuvix/string.h>
#include <nuvix/proc.h>
#include <nuvix/task.h>
#include <nuvix/page.h>
#include <nuvix/uaccess_arch.h>

#include "internal.h"

__always_inline __must_check __const
static inline size_t user_copy_width(uintptr_t uaddr, uintptr_t kaddr, size_t remaining)
{
	const size_t width = ALIGNMENT_OF2(uaddr, kaddr);

	if (width >= sizeof(u64) && remaining >= sizeof(u64))
		return sizeof(u64);
	if (width >= sizeof(u32) && remaining >= sizeof(u32))
		return sizeof(u32);
	if (width >= sizeof(u16) && remaining >= sizeof(u16))
		return sizeof(u16);
	return sizeof(u8);
}

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

int uaccess_txn_begin(struct uaccess_txn *txn)
{
	struct task_struct *task = current_task();
	struct mm_struct *mm;

	memset(txn, 0, sizeof(*txn));
	if (!task || !task->proc)
		return -EFAULT;

	/* The reference is taken under the proc lock only; mmap_lock is
	 * acquired after it is dropped.  Every later access in this
	 * transaction uses txn->mm, never current_task()->proc->mm. */
	mm = proc_mm_get(task->proc);
	if (!mm)
		return -EFAULT;

	txn->mm = mm;
	mm_lock(mm);
	return 0;
}

void uaccess_txn_end(struct uaccess_txn *txn)
{
	struct mm_struct *mm = txn->mm;

	if (!mm)
		return;

	txn->mm = NULL;
	/* Order is fixed: stop user access, drop mmap_lock, release
	 * mapping references outside the lock, then drop the MM
	 * reference so the final mm_put() cannot destroy under it. */
	mm_unlock(mm);
	mm_teardown_release(&txn->teardown);
	mm_put(mm);
}

__hot
size_t copy_to_user(void *to, const void *from, size_t n)
{
	struct uaccess_txn txn;
	uintptr_t user_addr = (uintptr_t)to;
	uintptr_t kernel_addr = (uintptr_t)from;
	size_t remaining = n;
	int ret;

	if (n == 0)
		return 0;
	if (!access_ok(to, n))
		return n;
	if (uaccess_txn_begin(&txn) < 0)
		return n;

	/* Fault-in (including COW splits) happens under the same
	 * mmap_lock as the copy itself, so concurrent munmap/mprotect
	 * cannot invalidate the range between probe and access. */
	ret = fault_in_user_range_locked(txn.mm, user_addr, n, USER_FAULT_WRITE,
					 &txn.teardown);
	if (ret == 0) {
		while (remaining != 0) {
			size_t width = user_copy_width(user_addr, kernel_addr,
						       remaining);

			switch (width) {
			case sizeof(u64):
				ret = put_user_u64(*(const u64 *)kernel_addr,
						   (volatile u64 *)user_addr);
				break;
			case sizeof(u32):
				ret = put_user_u32(*(const u32 *)kernel_addr,
						   (volatile u32 *)user_addr);
				break;
			case sizeof(u16):
				ret = put_user_u16(*(const u16 *)kernel_addr,
						   (volatile u16 *)user_addr);
				break;
			default:
				ret = put_user_u8(*(const u8 *)kernel_addr,
						  (volatile u8 *)user_addr);
				break;
			}
			if (ret < 0)
				break;

			user_addr += width;
			kernel_addr += width;
			remaining -= width;
		}
	}

	uaccess_txn_end(&txn);
	return remaining;
}

__hot
size_t copy_from_user(void *to, const void *from, size_t n)
{
	struct uaccess_txn txn;
	uintptr_t kernel_addr = (uintptr_t)to;
	uintptr_t user_addr = (uintptr_t)from;
	size_t remaining = n;
	int ret;

	if (n == 0)
		return 0;
	if (!access_ok(from, n))
		return n;
	if (uaccess_txn_begin(&txn) < 0)
		return n;

	ret = fault_in_user_range_locked(txn.mm, user_addr, n, USER_FAULT_READ,
					 &txn.teardown);
	if (ret == 0) {
		while (remaining != 0) {
			size_t width = user_copy_width(user_addr, kernel_addr,
						       remaining);

			switch (width) {
			case sizeof(u64): {
				u64 value;

				ret = get_user_u64(
					value, (const volatile u64 *)user_addr);
				if (ret == 0)
					*(u64 *)kernel_addr = value;
				break;
			}
			case sizeof(u32): {
				u32 value;

				ret = get_user_u32(
					value, (const volatile u32 *)user_addr);
				if (ret == 0)
					*(u32 *)kernel_addr = value;
				break;
			}
			case sizeof(u16): {
				u16 value;

				ret = get_user_u16(
					value, (const volatile u16 *)user_addr);
				if (ret == 0)
					*(u16 *)kernel_addr = value;
				break;
			}
			default: {
				u8 value;

				ret = get_user_u8(
					value, (const volatile u8 *)user_addr);
				if (ret == 0)
					*(u8 *)kernel_addr = value;
				break;
			}
			}
			if (ret < 0)
				break;

			user_addr += width;
			kernel_addr += width;
			remaining -= width;
		}
	}

	uaccess_txn_end(&txn);
	return remaining;
}

ssize_t strncpy_from_user(char *dst, const char *src, size_t maxlen)
{
	struct uaccess_txn txn;
	uintptr_t addr = (uintptr_t)src;
	size_t done = 0;

	if (!dst)
		return -EINVAL;
	if (!src)
		return -EFAULT;
	if (maxlen == 0)
		return -ENAMETOOLONG;
	if (!access_ok(src, maxlen))
		return -EFAULT;
	if (uaccess_txn_begin(&txn) < 0)
		return -EFAULT;

	/* One mmap_lock hold covers every page, so the scan cannot race
	 * with munmap()/mprotect() between pages.  Only pages actually
	 * reached by the scan are faulted in: a NUL inside the first
	 * page succeeds even when later pages are unmapped. */
	while (done < maxlen) {
		uintptr_t page = (addr + done) & PAGE_MASK;
		size_t offset = (addr + done) & (PAGE_SIZE - 1);
		size_t chunk = MIN(PAGE_SIZE - offset, maxlen - done);

		if (fault_in_user_range_locked(txn.mm, page, 1, USER_FAULT_READ,
					       &txn.teardown) < 0)
			goto fail;

		for (size_t i = 0; i < chunk; i++) {
			u8 value;

			if (get_user_u8(value,
					(const volatile u8 *)(addr + done)) < 0)
				goto fail;
			dst[done++] = (char)value;
			if (value == '\0') {
				uaccess_txn_end(&txn);
				return (ssize_t)(done - 1);
			}
		}
	}

	uaccess_txn_end(&txn);
	dst[maxlen - 1] = '\0';
	return -ENAMETOOLONG;

fail:
	uaccess_txn_end(&txn);
	return -EFAULT;
}
