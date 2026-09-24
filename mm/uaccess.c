/*
 * mm/uaccess.c - 用户空间内存访问
 */

#include <nuvix/mm.h>
#include <nuvix/math.h>
#include <nuvix/errno.h>
#include <nuvix/string.h>
#include <nuvix/task.h>
#include <nuvix/proc.h>
#include <nuvix/page.h>
#include <nuvix/uaccess.h>

#include "internal.h"

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
	mm = task->mm;
	if (!mm)
		return -EFAULT;

	access = write ? USER_FAULT_WRITE : USER_FAULT_READ;
	return fault_in_user_range(mm, (uintptr_t)addr, size, access);
}

static int uaccess_begin_ref(struct uaccess_txn *txn, struct mm_struct *mm)
{
	IFDEF(CONFIG_DEBUG_CONTEXT, BUG_ON(!wait_may_block());)
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

	/* Keep the address space alive across the whole access. */
	mm = task_mm_get(task);
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
	uint8_t *kernel = to_user ? (uint8_t *)from : to;

	if (!txn || !txn->mm || !access_ok((const void *)uaddr, n))
		return -EFAULT;
	while (n) {
		size_t offset = uaddr & (PAGE_SIZE - 1);
		size_t chunk = MIN(n, PAGE_SIZE - offset);
		pte_t *pte;
		uint8_t *data;
		int ret;

		ret = fault_in_urange_locked(txn->mm, uaddr, chunk,
			to_user ? USER_FAULT_WRITE : USER_FAULT_READ, &txn->teardown);
		if (ret < 0)
			return ret;
		/* This is the uaccess boundary: translation and permission remain
		 * stable through this page's copy, including a non-current mm. */
		pte = pt_lookup(txn->mm->pgroot, uaddr);
		if (!pte || (to_user ? !pte_uwrite(*pte) :
				      !pte_uread(*pte)))
			return -EFAULT;
		data = (uint8_t *)__va(pte_phys(*pte)) + offset;
		if (to_user) {
			struct vm_area_struct *vma = find_vma(txn->mm, uaddr);

			memcpy(data, kernel, chunk);
			if (vma && (vma->vm_flags & VM_EXEC)) {
				icache_flush();
				mm_flush_remote(txn->mm, true);
			}
		} else {
			memcpy(kernel, data, chunk);
		}
		uaddr += chunk;
		kernel += chunk;
		n -= chunk;
		if (n) {
			mm_unlock(txn->mm);
			mm_teardown_release(&txn->teardown);
			mm_lock(txn->mm);
		}
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

	/* Fault preparation may release mmap_lock. Each page is revalidated
	 * before scanning it; only pages reached by the string are accessed. */
	while (done < maxlen) {
		uintptr_t page = (addr + done) & PAGE_MASK;
		size_t offset = (addr + done) & (PAGE_SIZE - 1);
		size_t chunk = MIN(PAGE_SIZE - offset, maxlen - done);

		if (fault_in_urange_locked(txn.mm, page, 1, USER_FAULT_READ,
					       &txn.teardown) < 0)
			goto fail;

		for (size_t i = 0; i < chunk; i++) {
			u8 value;

			if (get_user(value, (const volatile u8 *)(addr + done)) < 0)
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
