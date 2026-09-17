#ifndef _NUVIX_MM_H
#define _NUVIX_MM_H

/**
 * @file mm.h
 * @brief 用户地址空间、mmap/brk ABI 支持与 uaccess 公共接口。
 */

#include <nuvix/compiler.h>
#include <nuvix/cleanup.h>
#include <nuvix/types.h>
#include <nuvix/fs.h>

struct anon_shared;
void mm_anon_get(struct anon_shared *anon);
void mm_anon_put(struct anon_shared *anon);

/* Bounded retirement storage: clearing mappings never allocates. */
#define MM_RELEASE_BATCH 32

struct mm_teardown {
	paddr_t release[MM_RELEASE_BATCH];
	size_t nr_release;
};

/** One explicit lifetime transaction for accesses to one address space. */
struct uaccess_txn {
	struct mm_struct *mm;
	struct mm_teardown teardown;

};

/**
 * @brief Create an empty, unpublished user address space.
 *
 * The returned address space is BUILDING.  The caller must finish all
 * construction and transfer its ownership to task_replace_mm(), which
 * performs the publication commit.
 * @return New mm with a user page table, or NULL on allocation failure.
 */
__must_check __malloc
struct mm_struct *mm_create_user(void);

/**
 * @brief Publish an address space into one thread-owned MM slot.
 *
 * The first publication commits a BUILDING address space as ACTIVE. Shared
 * address spaces may have more than one thread publication. Publication is
 * performed by task_replace_mm(); callers must hold the MM reference that
 * becomes the thread's ownership reference.
 */
__nonnull(1)
void mm_publish(struct mm_struct *mm);

/**
 * @brief Withdraw one thread publication of an address space.
 *
 * The final withdrawal changes the address space to RETIRING. References held
 * by the scheduler or in-flight operations may keep it alive until mm_put()
 * performs the final retirement.
 */
__nonnull(1)
void mm_unpublish(struct mm_struct *mm);

/**
 * @brief Take a reference to an mm_struct.
 * @param mm Address space to pin; may be NULL.
 */
void mm_get(struct mm_struct *mm);

/**
 * @brief Drop an mm_struct reference and destroy it at the last reference.
 * @param mm Address space to release; may be NULL.
 */
void mm_put(struct mm_struct *mm);

/* Reaper-only, sleepable; drains last references deferred by atomic callers.
 * No caller locks or address-space references are required. */
void mm_reap_retired(void);

CLEANUP_DEFINE(mm_ref, struct mm_struct *, if (_T) mm_put(_T));

/**
 * @brief Return the current number of references to an address space.
 * @param mm Address space to inspect.
 * @return Current reference count.
 */
__must_check __pure __nonnull(1)
int mm_refcount_read(const struct mm_struct *mm);

/**
 * @brief Duplicate a user address space for fork/clone.
 * @param oldmm Source address space.
 * @return New mm on success, or NULL.
 *
 * Private anonymous and file-backed mappings use copy-on-write sharing;
 * shared mappings retain their shared semantics.
 * Fixed mappings are inherited with their own mapping references; callers
 * must not reinstall them in the returned address space.
 */
__must_check __malloc
struct mm_struct *dup_mm(struct mm_struct *oldmm);

/**
 * @brief Return the architecture SATP value for entering a user mm.
 * @param mm Address space to inspect.
 * @return RISC-V satp value, or 0 for NULL.
 */
__must_check
uintptr_t mm_pgroot(const struct mm_struct *mm);


__must_check
int mm_map_page(struct mm_struct *mm, uintptr_t va, void *page, int prot);

/**
 * @brief Install one immutable fixed user page in an address space.
 * @param mm Unpublished address space being constructed.
 * @param va Page-aligned fixed user virtual address.
 * @param page Page borrowed with an additional reference for the mapping lifetime.
 * @param prot User access permissions.
 * @return 0 on success, or a negative errno.
 */
__must_check
int mm_install_fixed_page(struct mm_struct *mm, uintptr_t va, void *page, int prot);

__must_check
int mm_map_segment(struct mm_struct *mm, uintptr_t start, uintptr_t end, int prot);

__must_check
int mm_map_file_segment(struct mm_struct *mm, struct file *file,
                uintptr_t start, uintptr_t end, int prot, uint64_t file_offset);

__must_check
int mm_add_stack(struct mm_struct *mm, const void *stack,
			      size_t stack_size);

__must_check
int mm_finalize(struct mm_struct *mm, uintptr_t first_vaddr,
			     uintptr_t last_end);

/**
 * @brief Implement Linux brk heap query/growth semantics for one mm.
 * @param mm Address space whose heap VMA is modified.
 * @param addr Requested program break, or 0 to query current break.
 * @return Current program break after validation.
 */
__must_check
uintptr_t mm_brk(struct mm_struct *mm, uintptr_t addr);

/**
 * @brief Create an anonymous user mapping.
 * @param mm Address space that receives the mapping.
 * @param addr Requested base address, or 0 for kernel-selected placement.
 * @param length Mapping length in bytes.
 * @param prot Linux PROT_* bits.
 * @param flags Linux MAP_* bits accepted by nuvix.
 * @return Mapped user address, or a negative errno.
 */
__must_check
ssize_t mm_mmap(struct mm_struct *mm, uintptr_t addr, size_t length, int prot, int flags);

/**
 * @brief Create a file-backed user mapping.
 * @param mm Address space that receives the mapping.
 * @param addr Requested base address, or 0 for kernel-selected placement.
 * @param length Mapping length in bytes.
 * @param prot Linux PROT_* bits.
 * @param flags Linux MAP_* bits accepted by nuvix.
 * @param fd File descriptor resolved by the syscall layer.
 * @param offset File offset in bytes; must satisfy page-alignment rules.
 * @return Mapped user address, or a negative errno.
 */
__must_check
ssize_t mm_mmap_file(struct mm_struct *mm, uintptr_t addr, size_t length,
                int prot, int flags, int fd, uint64_t offset);

/**
 * @brief Remove mappings from a user address range.
 * @param mm Address space to update.
 * @param addr Page-aligned start address.
 * @param length Range length in bytes.
 * @return 0 on success, or a negative errno.
 */
__must_check
int mm_munmap(struct mm_struct *mm, uintptr_t addr, size_t length);

/**
 * @brief Change VMA and resident PTE permissions for a user range.
 * @param mm Address space to update.
 * @param addr Page-aligned range start.
 * @param len Range length in bytes.
 * @param prot Linux PROT_* permission mask.
 * @return 0 on success, or a negative errno.
 */
__must_check
int mm_mprotect(struct mm_struct *mm, uintptr_t addr, size_t len, int prot);

/**
 * @brief Validate that a user pointer range is inside user virtual memory.
 * @param addr User pointer start.
 * @param size Number of bytes in the range.
 * @return true when the range is a valid user address interval.
 */
__must_check __pure
bool access_ok(const void *addr, size_t size);

/**
 * @brief Probe that a user range is mapped and has requested access.
 * @param addr User pointer start.
 * @param size Number of bytes to probe.
 * @param write true when write permission is required.
 * @return 0 on success, or a negative errno.
 */
__must_check
int user_range_probe(const void *addr, size_t size, bool write);

/** Begin a transaction for an explicitly supplied address space. */
__must_check __nonnull(1, 2)
int uaccess_begin_mm(struct uaccess_txn *txn, struct mm_struct *mm);

/** End a transaction and release its MM/mapping references. */
__nonnull(1)
void uaccess_end(struct uaccess_txn *txn);

/** Copy user memory while @p txn owns its mmap lifetime window. */
__must_check __nonnull(1, 2, 3)
__access(write_only, 2, 4) __access(read_only, 3, 4)
int uaccess_copy_from(struct uaccess_txn *txn, void *to, const void *from, size_t n);

/** Copy to user memory while @p txn owns its mmap lifetime window. */
__must_check __nonnull(1, 2, 3)
__access(write_only, 2, 4) __access(read_only, 3, 4)
int uaccess_copy_to(struct uaccess_txn *txn, void *to, const void *from, size_t n);

/** Copy from an explicitly supplied address space in one transaction. */
__must_check __nonnull(1, 2, 3)
__access(write_only, 2, 4) __access(read_only, 3, 4)
int uaccess_copy_from_mm(struct mm_struct *mm, void *to, const void *from, size_t n);

/**
 * @brief Copy bytes from kernel memory to userspace.
 * @param to Destination user pointer.
 * @param from Source kernel pointer.
 * @param n Number of bytes requested.
 * @return 0 on success, or @p n if the requested range could not be copied.
 *
 * Copies proceed page by page. On failure earlier pages may have been
 * copied; the return value reports failure, not a transactional rollback.
 *
 * User memory must cross the kernel/userspace boundary through this helper or
 * an equivalent uaccess helper, never through direct dereference.
 */
__must_check __access(read_write, 1, 3) __access(read_only, 2, 3) __hot
size_t copy_to_user(void *to, const void *from, size_t n);

/**
 * @brief Copy bytes from userspace to kernel memory.
 * @param to Destination kernel pointer.
 * @param from Source user pointer.
 * @param n Number of bytes requested.
 * @return 0 on success, or @p n if the requested range could not be copied.
 *
 * Copies proceed page by page. On failure earlier pages may have been
 * copied; the return value reports failure, not a transactional rollback.
 */
__must_check __access(write_only, 1, 3) __access(read_only, 2, 3) __hot
size_t copy_from_user(void *to, const void *from, size_t n);

/**
 * @brief Copy a NUL-terminated string from userspace.
 * @param dst Kernel destination buffer.
 * @param src User source pointer.
 * @param maxlen Maximum bytes to copy, including the terminator.
 * @return String length excluding NUL, or a negative errno.
 */
__must_check __access(read_only, 2, 3) __access(write_only, 1, 3)
ssize_t	strncpy_from_user(char *dst, const char *src, size_t maxlen);

/**
 * @brief Resolve a user instruction/load/store page fault.
 * @param tf Trap frame holding faulting user context and scause/stval state.
 */
__nonnull(1)
void do_page_fault(struct trap_frame *tf);

/**
 * @brief Flush remote TLBs (and optionally icache) of CPUs running @p mm.
 * @param mm Address space to flush on remote CPUs. Caller holds mm->mmap_lock.
 * @param flush_icache Also shoot down instruction caches if true.
 *
 * The architecture broadcasts to online non-self CPUs and waits for their
 * acknowledgements. User root entry also flushes locally; no scheduler
 * snapshot is needed. The caller's local TLB is NOT flushed.
 */
#ifdef CONFIG_SMP
void mm_flush_remote(struct mm_struct *mm, bool flush_icache);
#else
/* Local invalidation remains the caller's responsibility in UP builds. */
static inline void mm_flush_remote(struct mm_struct *mm, bool flush_icache)
{
}
#endif

/**
 * @brief Flush the whole TLB on every other online CPU (kernel mappings).
 *
 * Sends a synchronous TLB shootdown IPI to all online non-self CPUs and waits
 * for their acks. Used for kernel-range (vmalloc) PTE updates.
 */
#ifdef CONFIG_SMP
void mm_flush_all(void);
#else
static inline void mm_flush_all(void)
{
}
#endif

#endif
