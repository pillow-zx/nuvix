/*
 * mm/page_fault.c - 缺页异常处理
 */

#include <kernel/mm.h>
#include <kernel/buddy.h>
#include <kernel/errno.h>
#include <kernel/exit.h>
#include <kernel/page_cache.h>
#include <kernel/printk.h>
#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/page.h>
#include <kernel/pgtable.h>
#include <kernel/trap.h>
#include <kernel/processor.h>

#include "internal.h"

static bool check_vma_permission(int access, struct vm_area_struct *vma)
{
	switch (access) {
	case USER_FAULT_EXEC:
		return (vma->vm_flags & VM_EXEC) != 0;
	case USER_FAULT_READ:
		return (vma->vm_flags & VM_READ) != 0;
	case USER_FAULT_WRITE:
		return (vma->vm_flags & VM_WRITE) != 0;
	default:
		return false;
	}
}

static bool pte_allows_fault(int access, pte_t pte)
{
	if (!pte_is_user_page(pte))
		return false;

	switch (access) {
	case USER_FAULT_EXEC:
		return pte_allows_user_exec(pte);
	case USER_FAULT_READ:
		return pte_allows_user_read(pte);
	case USER_FAULT_WRITE:
		return pte_allows_user_write(pte);
	default:
		return false;
	}
}

static void signal_or_panic_segv(struct trap_frame *tf, int code)
{
	if (trap_frame_from_user(tf)) {
		siginfo_t info = {0};

		info.si_signo = SIGSEGV;
		info.si_code = code;
		info.si_addr = (void *)trap_fault_addr(tf);
		if (force_signal_info(SIGSEGV, &info, current_task()) < 0)
			do_exit_signal(SIGSEGV);
		return;
	}

	panic("kernel page fault: type=%s addr=%p sepc=%p pid=%d",
	      trap_fault_name(tf), (void *)trap_fault_addr(tf),
	      (void *)trap_user_pc(tf), task_pid(current_task()));
}

static int fault_in_user_page_locked(struct mm_struct *mm, uintptr_t fault_addr,
				     int access, pte_t *fault_pte)
{
	struct vm_area_struct *vma = find_vma(mm, fault_addr);
	uintptr_t page_addr;
	pte_t *existing;

	if (!vma)
		return -EFAULT;
	if (!check_vma_permission(access, vma))
		return -EFAULT;

	page_addr = fault_addr & PAGE_MASK;
	existing = pagetable_lookup(mm->pgd, page_addr);

	if (existing && pte_is_present(*existing)) {
		if (pte_allows_fault(access, *existing)) {
			flush_tlb_page(page_addr);
			return 0;
		}

		if (fault_pte)
			*fault_pte = *existing;
		return -EFAULT;
	}

	if (vma->vm_file) {
		struct page_cache *file_page;
		uint64_t page_index;
		pgprot_t pte_flags;
		int mapping_error = 0;

		page_index = vma_page_index(vma, page_addr);
		file_page = page_cache_get_mapping(
			&vma->vm_file->f_inode->i_pages, page_index,
			PAGE_CACHE_READ |
			((vma->vm_shared && (vma->vm_flags & VM_WRITE)) ?
				 PAGE_CACHE_CREATE : 0),
			&mapping_error);
		if (!file_page && mapping_error != -ENODATA)
			return mapping_error ? mapping_error : -EIO;

		if (!file_page) {
			void *zero_page = get_free_page(0, ALLOC_NOWAIT);
			int ret;

			if (!zero_page)
				return -ENOMEM;
			memset(zero_page, 0, PAGE_SIZE);
			if (page_addr < vma->vm_start)
				memset(zero_page, 0, vma->vm_start - page_addr);
			if (page_addr + PAGE_SIZE > vma->vm_end) {
				uintptr_t keep = vma->vm_end - page_addr;

				memset((uint8_t *)zero_page + keep, 0,
				       PAGE_SIZE - keep);
			}
			ret = map_page(mm->pgd, page_addr,
				       __pa((uintptr_t)zero_page),
				       vma_flags_to_pte(vma->vm_flags));
			if (ret < 0) {
				free_page(zero_page, 0);
				return ret;
			}
			flush_tlb_page(page_addr);
			return 0;
		}

		pte_flags = vma_flags_to_pte(vma->vm_flags);
		if (vma->vm_shared) {
			int ret = map_page(
				mm->pgd, page_addr,
				__pa((uintptr_t)page_cache_data(file_page)),
				pte_flags);
			if (ret < 0) {
				page_cache_put_page(file_page);
				return ret;
			}
			flush_tlb_page(page_addr);
			/* The PTE owns the cache reference until unmap. */
			return 0;
		}

		void *page = get_free_page(0, ALLOC_NOWAIT);
		if (!page) {
			page_cache_put_page(file_page);
			return -ENOMEM;
		}

		memcpy(page, page_cache_data(file_page), PAGE_SIZE);
		if (page_addr < vma->vm_start)
			memset(page, 0, vma->vm_start - page_addr);
		if (page_addr + PAGE_SIZE > vma->vm_end) {
			uintptr_t keep = vma->vm_end - page_addr;

			memset((uint8_t *)page + keep, 0, PAGE_SIZE - keep);
		}
		page_cache_put_page(file_page);
		int ret = map_page(mm->pgd, page_addr, __pa((uintptr_t)page),
				   pte_flags);
		if (ret < 0) {
			free_page(page, 0);
			return ret;
		}
		flush_tlb_page(page_addr);
		return 0;
	}

	void *page = get_free_page(0, ALLOC_NOWAIT);
	if (!page)
		return -ENOMEM;

	memset(page, 0, PAGE_SIZE);
	int ret = map_page(mm->pgd, page_addr, __pa((uintptr_t)page),
			   vma_flags_to_pte(vma->vm_flags));
	if (ret < 0) {
		free_page(page, 0);
		return ret;
	}
	flush_tlb_page(page_addr);
	return 0;
}

int fault_in_user_range(struct mm_struct *mm, uintptr_t addr, size_t size,
			int access)
{
	uintptr_t range_end;

	if (size == 0)
		return 0;
	if (!mm)
		return -EFAULT;
	if (!access_ok((const void *)addr, size))
		return -EFAULT;

	range_end = addr + size;
	for (uintptr_t cursor = addr; cursor < range_end;) {
		struct vm_area_struct *vma;
		uintptr_t segment_end;
		uintptr_t va;
		uintptr_t page_end;
		int ret = 0;

		mm_lock(mm);
		vma = find_vma(mm, cursor);
		if (!vma) {
			mm_unlock(mm);
			return -EFAULT;
		}
		if (!check_vma_permission(access, vma)) {
			mm_unlock(mm);
			return -EFAULT;
		}

		segment_end = vma->vm_end < range_end ? vma->vm_end : range_end;
		if (segment_end <= cursor) {
			mm_unlock(mm);
			return -EFAULT;
		}

		va = cursor & PAGE_MASK;
		page_end = mm_page_align_up(segment_end);
		while (va < page_end) {
			uintptr_t fault_addr = va < cursor ? cursor : va;

			ret = fault_in_user_page_locked(mm, fault_addr, access,
							NULL);
			if (ret < 0)
				break;
			va += PAGE_SIZE;
		}
		mm_unlock(mm);

		if (ret < 0)
			return ret;

		cursor = segment_end;
	}

	return 0;
}

void do_page_fault(struct trap_frame *tf)
{
	vaddr_t fault_addr = trap_fault_addr(tf);
	const char *fault_name = trap_fault_name(tf);
	bool from_user_mode = trap_frame_from_user(tf);
	struct mm_struct *mm = task_mm(current_task());
	int access = (int)trap_fault_access(tf);
	pte_t fault_pte = 0;
	int ret;

	if (!mm) {
		panic("page fault in kernel thread: type=%s addr=%p "
		      "sepc=%p",
		      fault_name, (void *)fault_addr, (void *)trap_user_pc(tf));
	}

	mm_lock(mm);
	ret = fault_in_user_page_locked(mm, fault_addr, access, &fault_pte);
	mm_unlock(mm);

	if (ret == 0)
		return;

	if (ret == -ENOMEM) {
		pr_err("page fault: OOM at addr=%p pid=%d\n",
		       (void *)fault_addr, task_pid(current_task()));
		do_exit(1);
		unreachable();
	}

	struct vm_area_struct *vma;

	mm_lock(mm);
	vma = find_vma(mm, fault_addr);
	if (!vma) {
		mm_unlock(mm);
		pr_warn("page fault: illegal access (no VMA) "
			"type=%s addr=%p sepc=%p origin=%s pid=%d\n",
			fault_name, (void *)fault_addr,
			(void *)trap_user_pc(tf),
			from_user_mode ? "user" : "kernel",
			task_pid(current_task()));
		signal_or_panic_segv(tf, SEGV_MAPERR);
		return;
	}

	if (!check_vma_permission(access, vma)) {
		uint32_t vm_flags = vma->vm_flags;

		mm_unlock(mm);
		pr_warn("page fault: permission denied "
			"type=%s addr=%p vma_flags=0x%x sepc=%p "
			"origin=%s pid=%d\n",
			fault_name, (void *)fault_addr, vm_flags,
			(void *)trap_user_pc(tf),
			from_user_mode ? "user" : "kernel",
			task_pid(current_task()));
		signal_or_panic_segv(tf, SEGV_ACCERR);
		return;
	}
	mm_unlock(mm);

	pr_warn("page fault: mapped page permission denied "
		"type=%s addr=%p pte=0x%lx sepc=%p origin=%s pid=%d\n",
		fault_name, (void *)fault_addr, (size_t)fault_pte,
		(void *)trap_user_pc(tf), from_user_mode ? "user" : "kernel",
		task_pid(current_task()));
	signal_or_panic_segv(tf, SEGV_ACCERR);
}
