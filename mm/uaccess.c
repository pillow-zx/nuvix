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
	vaddr_t end;

	if (size == 0)
		return true;
	if (check_add_overflow(a, (vaddr_t)size, &end) || end > TASK_SIZE)
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

static int uaccess_begin_ref(struct uaccess_txn *txn, struct mm_struct *mm)
{
	memset(txn, 0, sizeof(*txn));
	txn->mm = mm;
	mm_lock(mm);
	return 0;
}

int uaccess_begin_mm(struct uaccess_txn *txn, struct mm_struct *mm)
{
	mm_get(mm);
	return uaccess_begin_ref(txn, mm);
}

static int uaccess_begin_current(struct uaccess_txn *txn)
{
	struct task_struct *task = current_task();
	struct mm_struct *mm;

	if (!task || !task->proc)
		return -EFAULT;

	/* The reference is taken under the proc lock only; mmap_lock is
	 * acquired after it is dropped.  Every later access in this
	 * transaction uses txn->mm, never current_task()->proc->mm. */
	mm = proc_mm_get(task->proc);
	if (!mm)
		return -EFAULT;

	return uaccess_begin_ref(txn, mm);
}

void uaccess_end(struct uaccess_txn *txn)
{
	struct mm_struct *mm = txn->mm;

	if (!mm)
		return;

	txn->mm = NULL;
	mm_unlock(mm);
	mm_teardown_release(&txn->teardown);
	mm_put(mm);
}

static int uaccess_copy(struct uaccess_txn *txn, void *to, const void *from,
			size_t n, bool to_user)
{
	uintptr_t uaddr = (uintptr_t)(to_user ? to : from);
	uintptr_t kaddr = (uintptr_t)(to_user ? from : to);
	size_t remaining = n;
	int ret;

	if (!txn || !txn->mm || !to || !from)
		return -EFAULT;
	if (n == 0)
		return 0;
	if (!access_ok((const void *)uaddr, n))
		return -EFAULT;
	ret = fault_in_user_range_locked(
		txn->mm, uaddr, n,
		to_user ? USER_FAULT_WRITE : USER_FAULT_READ, &txn->teardown);
	if (ret < 0)
		return ret;

	while (remaining != 0) {
		size_t width = user_copy_width(uaddr, kaddr, remaining);

		if (to_user) {
			switch (width) {
			case sizeof(u64):
				ret = put_user_u64(*(const u64 *)kaddr,
						   (volatile u64 *)uaddr);
				break;
			case sizeof(u32):
				ret = put_user_u32(*(const u32 *)kaddr,
						   (volatile u32 *)uaddr);
				break;
			case sizeof(u16):
				ret = put_user_u16(*(const u16 *)kaddr,
						   (volatile u16 *)uaddr);
				break;
			default:
				ret = put_user_u8(*(const u8 *)kaddr,
						  (volatile u8 *)uaddr);
				break;
			}
		} else {
			switch (width) {
			case sizeof(u64): {
				u64 value;

				ret = get_user_u64(
					value, (const volatile u64 *)uaddr);
				if (ret == 0)
					*(u64 *)kaddr = value;
				break;
			}
			case sizeof(u32): {
				u32 value;

				ret = get_user_u32(
					value, (const volatile u32 *)uaddr);
				if (ret == 0)
					*(u32 *)kaddr = value;
				break;
			}
			case sizeof(u16): {
				u16 value;

				ret = get_user_u16(
					value, (const volatile u16 *)uaddr);
				if (ret == 0)
					*(u16 *)kaddr = value;
				break;
			}
			default: {
				u8 value;

				ret = get_user_u8(
					value, (const volatile u8 *)uaddr);
				if (ret == 0)
					*(u8 *)kaddr = value;
				break;
			}
			}
		}
		if (ret < 0)
			return ret;
		uaddr += width;
		kaddr += width;
		remaining -= width;
	}
	return 0;
}

int uaccess_copy_from(struct uaccess_txn *txn, void *to, const void *from,
		      size_t n)
{
	return uaccess_copy(txn, to, from, n, false);
}

int uaccess_copy_to(struct uaccess_txn *txn, void *to, const void *from,
		    size_t n)
{
	return uaccess_copy(txn, to, from, n, true);
}

int uaccess_copy_from_mm(struct mm_struct *mm, void *to, const void *from,
			 size_t n)
{
	struct uaccess_txn txn;
	int ret;

	ret = uaccess_begin_mm(&txn, mm);
	if (ret < 0)
		return ret;
	ret = uaccess_copy_from(&txn, to, from, n);
	uaccess_end(&txn);
	return ret;
}

static int uaccess_prepare_u32(struct uaccess_txn *txn,
			       const volatile void *addr)
{
	if (!txn || !txn->mm)
		return -EFAULT;
	if (((uintptr_t)addr & (sizeof(uint32_t) - 1)) != 0)
		return -EINVAL;
	return fault_in_user_range_locked(txn->mm, (uintptr_t)addr,
					  sizeof(uint32_t), USER_FAULT_WRITE,
					  &txn->teardown);
}

int uaccess_cmpxchg_u32(struct uaccess_txn *txn, volatile uint32_t *addr,
			uint32_t expected, uint32_t desired, uint32_t *observed)
{
	int ret;

	if (!observed)
		return -EFAULT;
	ret = uaccess_prepare_u32(txn, addr);
	if (ret < 0)
		return ret;
	return user_u32_cmpxchg_acq_rel(addr, expected, desired, observed);
}

int uaccess_load_u32(struct uaccess_txn *txn, const volatile uint32_t *addr,
		     uint32_t *value)
{
	int ret;

	if (!value)
		return -EFAULT;
	ret = uaccess_prepare_u32(txn, addr);
	if (ret < 0)
		return ret;
	return user_u32_load_acquire(addr, value);
}

size_t copy_to_user(void *to, const void *from, size_t n)
{
	struct uaccess_txn txn;
	int ret;

	if (n == 0)
		return 0;
	if (uaccess_begin_current(&txn) < 0)
		return n;
	ret = uaccess_copy_to(&txn, to, from, n);
	uaccess_end(&txn);
	return ret < 0 ? n : 0;
}

size_t copy_from_user(void *to, const void *from, size_t n)
{
	struct uaccess_txn txn;
	int ret;

	if (n == 0)
		return 0;
	if (uaccess_begin_current(&txn) < 0)
		return n;
	ret = uaccess_copy_from(&txn, to, from, n);
	uaccess_end(&txn);
	return ret < 0 ? n : 0;
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
	/* maxlen bounds the scan; it is not necessarily the number of bytes
	 * accessed when the string terminates before the user-space limit. */
	if (addr >= TASK_SIZE)
		return -EFAULT;
	if (uaccess_begin_current(&txn) < 0)
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
				uaccess_end(&txn);
				return (ssize_t)(done - 1);
			}
		}
	}

	uaccess_end(&txn);
	dst[maxlen - 1] = '\0';
	return -ENAMETOOLONG;

fail:
	uaccess_end(&txn);
	return -EFAULT;
}
