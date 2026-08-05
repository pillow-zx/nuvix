/*
 * syscall/sys_mm.c - 内存相关系统调用
 */

#include <kernel/errno.h>
#include <kernel/syscall.h>
#include <kernel/mm.h>
#include <kernel/page.h>
#include <kernel/task.h>
#include <uapi/mman.h>
#include <kernel/trap.h>

ssize_t sys_brk(struct trap_frame *tf)
{
	uintptr_t addr = (uintptr_t)syscall_arg(tf, 0);

	return (ssize_t)mm_brk(current_task()->proc ? current_task()->proc->mm : NULL,
			       addr);
}

/*
 * SYSCALL_SUPPORT(B): mmap
 * Current: maps anonymous or regular file-backed MAP_PRIVATE/MAP_SHARED VMAs;
 * supports MAP_SHARED_VALIDATE, MAP_FIXED_NOREPLACE, and best-effort
 * MAP_POPULATE; accepts DENYWRITE/EXECUTABLE/NORESERVE/STACK as no-op
 * compatibility flags.
 * Unsupported errno: unknown/unsupported MAP flags return -EINVAL, except
 * unsupported MAP_SHARED_VALIDATE extension flags return -EOPNOTSUPP; invalid
 * prot/type combinations or bad alignment return -EINVAL; overlap with
 * MAP_FIXED_NOREPLACE returns -EEXIST; bad fd returns -EBADF; permission
 * mismatch returns -EACCES.
 * Future: add MAP_LOCKED only when real resident pinning semantics exist.
 */
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
 * Future: add cross-VMA and exec-cache/W^X coverage.
 */
ssize_t sys_mprotect(struct trap_frame *tf)
{
	uintptr_t addr = (uintptr_t)syscall_arg(tf, 0);
	size_t length = (size_t)syscall_arg(tf, 1);
	int prot = (int)syscall_arg(tf, 2);

	return mm_mprotect(current_task()->proc ? current_task()->proc->mm : NULL,
			   addr, length, prot);
}

/*
 * SYSCALL_SUPPORT(B): mremap
 * Current: supports basic resize and MAYMOVE/FIXED movement of mmap VMAs.
 * Unsupported errno: MREMAP_DONTUNMAP, invalid flag combinations, or bad
 * alignment return -EINVAL; no-move growth failure returns -ENOMEM.
 * Future: add DONTUNMAP only with a precise source-retention contract.
 */
ssize_t sys_mremap(struct trap_frame *tf)
{
	uintptr_t old_addr = (uintptr_t)syscall_arg(tf, 0);
	size_t old_size = (size_t)syscall_arg(tf, 1);
	size_t new_size = (size_t)syscall_arg(tf, 2);
	int flags = (int)syscall_arg(tf, 3);
	uintptr_t new_addr = (uintptr_t)syscall_arg(tf, 4);

	return mm_mremap(current_task()->proc ? current_task()->proc->mm : NULL,
			 old_addr, old_size, new_size,
			 flags, new_addr);
}

/*
 * SYSCALL_SUPPORT(B): msync
 * Current: validates mapped ranges, marks resident shared file pages dirty,
 * and syncs through VFS/page cache for MS_SYNC; MS_ASYNC records dirty state
 * without waiting and MS_INVALIDATE is accepted as a no-op.
 * Unsupported errno: unknown or conflicting flags return -EINVAL; unmapped
 * ranges return -ENOMEM.
 * Future: add range-limited writeback/invalidation if page cache grows it.
 */
ssize_t sys_msync(struct trap_frame *tf)
{
	uintptr_t addr = (uintptr_t)syscall_arg(tf, 0);
	size_t length = (size_t)syscall_arg(tf, 1);
	int flags = (int)syscall_arg(tf, 2);

	return mm_msync(current_task()->proc ? current_task()->proc->mm : NULL,
			addr, length, flags);
}

/*
 * SYSCALL_SUPPORT(C): mlock
 * Current: validates mapped ranges and faults in readable resident pages.
 * Unsupported errno: invalid ranges return -EINVAL; unmapped ranges return
 * -ENOMEM; no real pinning or rlimit accounting is implemented.
 * Future: avoid advertising complete resident pin semantics.
 */
ssize_t sys_mlock(struct trap_frame *tf)
{
	uintptr_t addr = (uintptr_t)syscall_arg(tf, 0);
	size_t len = (size_t)syscall_arg(tf, 1);
	struct mm_struct *mm = current_task()->proc ? current_task()->proc->mm : NULL;

	if (!mm)
		return -EINVAL;

	return mm_mlock(mm, addr, len);
}

/*
 * SYSCALL_SUPPORT(C): munlock
 * Current: validates that the range is mapped but stores no lock state.
 * Unsupported errno: invalid ranges return -EINVAL; unmapped ranges return
 * -ENOMEM.
 * Future: pair with real mlock state when resident pinning exists.
 */
ssize_t sys_munlock(struct trap_frame *tf)
{
	uintptr_t addr = (uintptr_t)syscall_arg(tf, 0);
	size_t len = (size_t)syscall_arg(tf, 1);
	struct mm_struct *mm = current_task()->proc ? current_task()->proc->mm : NULL;

	if (!mm)
		return -EINVAL;

	return mm_munlock(mm, addr, len);
}

/*
 * SYSCALL_SUPPORT(B): madvise
 * Current: NORMAL/RANDOM/SEQUENTIAL/WILLNEED/FREE are accepted as no-op hints;
 * DONTNEED drops resident anonymous, MAP_PRIVATE file, and MAP_SHARED file
 * pages while keeping VMAs intact.
 * Unsupported errno: unknown advice, including REMOVE, returns -EINVAL;
 * unmapped ranges return -ENOMEM.
 * Future: add REMOVE/POPULATE-like advice only with backing-store semantics.
 */
ssize_t sys_madvise(struct trap_frame *tf)
{
	uintptr_t addr = (uintptr_t)syscall_arg(tf, 0);
	size_t len = (size_t)syscall_arg(tf, 1);
	int advice = (int)syscall_arg(tf, 2);

	switch (advice) {
	case MADV_NORMAL:
	case MADV_RANDOM:
	case MADV_SEQUENTIAL:
	case MADV_WILLNEED:
	case MADV_DONTNEED:
	case MADV_FREE:
		break;
	default:
		return -EINVAL;
	}

	return mm_madvise(current_task()->proc ? current_task()->proc->mm : NULL,
			 addr, len, advice);
}

/*
 * SYSCALL_SUPPORT(B): mincore
 * Current: reports resident PTE state for mapped anonymous and file-backed
 * user pages.
 * Unsupported errno: unaligned or overflowing ranges return -EINVAL; NULL vec
 * returns -EFAULT; file-cache-only residency is not reported.
 * Future: keep this tied to real resident user mappings unless page-cache
 * residency becomes observable by design.
 */
ssize_t sys_mincore(struct trap_frame *tf)
{
	uintptr_t addr = (uintptr_t)syscall_arg(tf, 0);
	size_t len = (size_t)syscall_arg(tf, 1);
	unsigned char *uvec = (unsigned char *)syscall_arg(tf, 2);
	struct mm_struct *mm;
	uintptr_t end, va;
	size_t npages, i;

	unsigned char kbuf[256];
	size_t batch, done = 0;

	if (addr & (PAGE_SIZE - 1))
		return -EINVAL;
	if (len == 0)
		return 0;
	if (!uvec)
		return -EFAULT;
	if (len > TASK_SIZE)
		return -EINVAL;

	len = (len + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);
	end = addr + len;
	if (end < addr || end > TASK_SIZE)
		return -EINVAL;

	npages = len / PAGE_SIZE;

	mm = current_task()->proc ? current_task()->proc->mm : NULL;
	if (!mm)
		return -EINVAL;

	while (done < npages) {
		batch = npages - done;
		if (batch > sizeof(kbuf))
			batch = sizeof(kbuf);

		for (i = 0; i < batch; i++) {
			bool resident;
			int ret;

			va = addr + (done + i) * PAGE_SIZE;
			ret = mm_user_page_resident(mm, va, &resident);
			if (ret < 0)
				return ret;
			kbuf[i] = resident ? 1 : 0;
		}

		if (copy_to_user(uvec + done, kbuf, batch) != 0)
			return -EFAULT;

		done += batch;
	}
	return 0;
}
