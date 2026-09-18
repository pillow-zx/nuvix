/*
 * mm/page_fault.c - 缺页异常处理
 */

#include <nuvix/mm.h>
#include <nuvix/math.h>
#include <nuvix/buddy.h>
#include <nuvix/errno.h>
#include <nuvix/exit.h>
#include <nuvix/fdtable.h>
#include <nuvix/page_cache.h>
#include <nuvix/printk.h>
#include <nuvix/signal.h>
#include <nuvix/task.h>
#include <nuvix/proc.h>
#include <nuvix/page.h>
#include <nuvix/pgtable.h>
#include <nuvix/trap.h>
#include <nuvix/processor.h>

#include "arch/config.h"
#include "internal.h"

__pure
static inline bool check_vma_permission(int access, struct vm_area_struct *vma)
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

__pure
static inline bool pte_allows_fault(int access, pte_t pte)
{
	if (!pte_upage(pte))
		return false;

	switch (access) {
	case USER_FAULT_EXEC:
		return pte_uexec(pte);
	case USER_FAULT_READ:
		return pte_uread(pte);
	case USER_FAULT_WRITE:
		return pte_uwrite(pte);
	default:
		return false;
	}
}

__cold
static void signal_or_panic_segv(struct trap_frame *tf, int code)
{
	if (trap_frame_from_user(tf)) {
		siginfo_t info = {0};

		info.si_signo = SIGSEGV;
		info.si_code = code;
		info.si_addr = (void *)trap_fault_addr(tf);
		if (sig_force_info(SIGSEGV, &info, current_task()) < 0)
			do_exit_signal(SIGSEGV);
		return;
	}

	panic("kernel page fault: type=%s addr=%p sepc=%p pid=%d",
	      trap_fault_name(tf), (void *)trap_fault_addr(tf),
	      (void *)trap_user_pc(tf),
	      current_task()->proc ? current_task()->proc->pid->nr : 0);
}

/* Enters and returns with mmap_lock held. Preparation drops that lock;
 * only referenced backing/content and scalar snapshots survive the drop. */
static int fault_in_user_page_locked(struct mm_struct *mm, uintptr_t fault_addr,
				     int access, pte_t *fault_pte,
				     struct mm_teardown *teardown)
{
	uintptr_t va = fault_addr & PAGE_MASK;

	for (;;) {
		struct vm_area_struct *vma = find_vma(mm, fault_addr);
		struct mm_page_slot *slot;
		struct page *source = NULL, *page = NULL;
		struct anon_shared *anon;
		struct file *file;
		struct pgcache *cached = NULL;
		uint64_t sequence, index;
		bool cow = false, private = false;
		pgroot_t prot;
		pte_t *pte;
		int ret = 0;

		if (!vma || !check_vma_permission(access, vma))
			return -EFAULT;
		pte = pt_lookup(mm->pgroot, va);
		if (pte && pte_allows_fault(access, *pte))
			return 0;
		if (fault_pte)
			*fault_pte = pte ? *pte : 0;
		prot = vma_flags_to_pte(vma->vm_flags);
		sequence = mm->map_sequence;
		index = vma_page_index(vma, va);
		anon = vma->vm_anon;
		file = vma->vm_file;
		anon_shared_get(anon);
		file_get(file);
		slot = anon ? NULL : mm_private_find(mm, va);
		if (slot) {
			source = slot->page;
			cow = slot->cow;
			page_get(source);
		}
		mm_unlock(mm);

		if (file && index >= DIV_ROUND_UP(file->f_inode->i_size, PAGE_SIZE)) {
			ret = -EIO;
		} else if (anon) {
			page = anon_shared_page(anon, index);
		} else if (source && (access != USER_FAULT_WRITE || !cow)) {
			page = source;
			page_get(page);
			if (cow)
				prot = pgroot_ro(prot);
		} else if (!source && !file && access != USER_FAULT_WRITE) {
			page = mm_zero_page();
			prot = pgroot_ro(prot);
		} else {
			const void *contents = source ? page_to_virt(source) : NULL;

			if (!source && file) {
				cached = pgcache_get_mapping(&file->f_inode->i_pages,
					index, PAGE_CACHE_READ, &ret);
				if (!cached && ret == -ENODATA)
					ret = 0;
				if (cached)
					contents = page_cache_data(cached);
			}
			if (!ret && file && !source && access != USER_FAULT_WRITE) {
				page = cached ? virt_to_page(page_cache_data(cached)) :
					mm_zero_page();
				if (cached)
					page_get(page);
				prot = pgroot_ro(prot);
			} else if (!ret) {
				void *data = get_page(0, ALLOC_NOWAIT);

				if (data) {
					if (contents)
						memcpy(data, contents, PAGE_SIZE);
					else
						memset(data, 0, PAGE_SIZE);
					page = virt_to_page(data);
					private = true;
				}
			}
		}
		if (cached)
			pgcache_put_page(cached);
		if (!page && !ret)
			ret = -ENOMEM;

		mm_lock(mm);
		vma = find_vma(mm, fault_addr);
		slot = anon ? NULL : mm_private_find(mm, va);
		if (sequence != mm->map_sequence || !vma ||
		    (slot ? slot->page : NULL) != source ||
		    (slot && slot->cow != cow)) {
			if (page)
				page_put(page);
			if (source)
				page_put(source);
			file_put(file);
				anon_shared_put(anon);
			continue;
		}
		if (source)
			page_put(source);
		file_put(file);
		anon_shared_put(anon);
		if (ret < 0)
			return ret;
		pte = pt_lookup(mm->pgroot, va);
		if (pte && pte_allows_fault(access, *pte)) {
			page_put(page);
			return 0;
		}
		if (private) {
			ret = mm_private_set(mm, va, page, false);
			if (ret < 0) {
				page_put(page);
				return ret;
			}
		}
		if (pte && pte_upage(*pte)) {
			replace_pte_locked(mm, vma, va, pte,
				pte_make(__pa((uintptr_t)page_to_virt(page)), prot),
				PTE_TO_PA(*pte), teardown);
		} else {
			ret = map_page(mm->pgroot, va,
				__pa((uintptr_t)page_to_virt(page)), prot);
			if (ret < 0)
				page_put(page);
			else
				tlb_flush_page(va);
		}
		if (!ret && (vma->vm_flags & VM_EXEC)) {
			icache_flush();
			mm_flush_remote(mm, true);
		}
		return ret;
	}
}

/* Caller holds mm->mmap_lock and owns @teardown release after unlocking.
 * The whole range must be covered: VMA segments are checked for coverage
 * and permission, then faulted in page by page. */
__hot
int fault_in_urange_locked(struct mm_struct *mm, uintptr_t addr,
				     size_t size, int access,
				     struct mm_teardown *teardown)
{
	uintptr_t range_end;

	if (size == 0)
		return 0;
	if (unlikely(!access_ok((const void *)addr, size)))
		return -EFAULT;

	range_end = addr + size;
	for (uintptr_t cursor = addr; cursor < range_end;) {
		int ret = fault_in_user_page_locked(mm, cursor, access, NULL,
						 teardown);

		if (ret < 0)
			return ret;
		cursor = MIN(ALIGN_UP(cursor + 1, PAGE_SIZE), range_end);
	}

	return 0;
}

__hot
int fault_in_user_range(struct mm_struct *mm, uintptr_t addr, size_t size,
			      int access)
{
	struct mm_teardown teardown = {0};
	int ret;

	mm_lock(mm);
	ret = fault_in_urange_locked(mm, addr, size, access, &teardown);
	mm_unlock(mm);
	mm_teardown_release(&teardown);
	return ret;
}

__hot
void do_page_fault(struct trap_frame *tf)
{
	vaddr_t fault_addr = trap_fault_addr(tf);
	const char *fault_name = trap_fault_name(tf);
	bool from_user_mode = trap_frame_from_user(tf);
	struct mm_struct *mm = current_task()->mm;
	int access = (int)trap_fault_access(tf);

	/* The trap-access and user-fault enums are compared after a naked
	 * cast below; their values must agree by construction, not luck. */
	static_assert(TRAP_ACCESS_READ == USER_FAULT_READ &&
		      TRAP_ACCESS_WRITE == USER_FAULT_WRITE &&
		      TRAP_ACCESS_EXEC == USER_FAULT_EXEC,
		      "trap_access_type and USER_FAULT_* must agree");
	pte_t fault_pte = 0;
	struct mm_teardown teardown = {0};
	int ret;

	if (!mm) {
		panic("page fault in kernel thread: type=%s addr=%p "
		      "sepc=%p",
		      fault_name, (void *)fault_addr, (void *)trap_user_pc(tf));
	}

	mm_lock(mm);
	ret = fault_in_user_page_locked(mm, fault_addr, access, &fault_pte,
					&teardown);
	mm_unlock(mm);
	mm_teardown_release(&teardown);

	if (ret == 0)
		return;

	if (ret == -ENOMEM) {
		pr_err("page fault: OOM at addr=%p pid=%d\n",
		       (void *)fault_addr,
		       current_task()->proc ? current_task()->proc->pid->nr : 0);
		do_exit(1);
		unreachable();
	}

	struct vm_area_struct *vma;

	mm_lock(mm);
	vma = find_vma(mm, fault_addr);
	if (unlikely(!vma)) {
		mm_unlock(mm);
		pr_warn("page fault: illegal access (no VMA) "
			"type=%s addr=%p sepc=%p origin=%s pid=%d\n",
			fault_name, (void *)fault_addr,
			(void *)trap_user_pc(tf),
			from_user_mode ? "user" : "kernel",
			current_task()->proc ? current_task()->proc->pid->nr : 0);
		signal_or_panic_segv(tf, SEGV_MAPERR);
		return;
	}

	if (unlikely(!check_vma_permission(access, vma))) {
		uint32_t vm_flags = vma->vm_flags;

		mm_unlock(mm);
		pr_warn("page fault: permission denied "
			"type=%s addr=%p vma_flags=0x%x sepc=%p "
			"origin=%s pid=%d\n",
			fault_name, (void *)fault_addr, vm_flags,
			(void *)trap_user_pc(tf),
			from_user_mode ? "user" : "kernel",
			 current_task()->proc ? current_task()->proc->pid->nr : 0);
		signal_or_panic_segv(tf, SEGV_ACCERR);
		return;
	}
	mm_unlock(mm);

	pr_warn("page fault: mapped page permission denied "
		"type=%s addr=%p pte=0x%lx sepc=%p origin=%s pid=%d\n",
		fault_name, (void *)fault_addr, (size_t)fault_pte,
		(void *)trap_user_pc(tf), from_user_mode ? "user" : "kernel",
		current_task()->proc ? current_task()->proc->pid->nr : 0);
	signal_or_panic_segv(tf, SEGV_ACCERR);
}
