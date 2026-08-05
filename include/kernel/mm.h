#ifndef _CUTEOS_KERNEL_MM_H
#define _CUTEOS_KERNEL_MM_H

/**
 * @file mm.h
 * @brief 用户地址空间、mmap/brk ABI 支持与 uaccess 公共接口。
 */

#include <kernel/compiler.h>
#include <kernel/types.h>
#include <kernel/fs.h>

/**
 * @brief Create an empty user address space.
 * @return New mm with a user page table, or NULL on allocation failure.
 */
__must_check
struct mm_struct *mm_create_user(void);

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
void mm_membarrier_register(struct mm_struct *mm, uint32_t cmd);

__must_check
uint32_t mm_membarrier_registrations(const struct mm_struct *mm);

/**
 * @brief Duplicate a user address space for fork/clone.
 * @param oldmm Source address space.
 * @return New mm on success, or NULL.
 *
 * The current implementation copies user mappings rather than installing a
 * copy-on-write contract.
 */
__must_check
struct mm_struct *dup_mm(struct mm_struct *oldmm);

/**
 * @brief Return the architecture SATP value for entering a user mm.
 * @param mm Address space to inspect.
 * @return RISC-V satp value, or 0 for NULL.
 */
__must_check
uintptr_t mm_pgroot(const struct mm_struct *mm);

__must_check
int mm_user_page_resident(struct mm_struct *mm, uintptr_t addr, bool *resident);

__must_check
int mm_map_page(struct mm_struct *mm, uintptr_t va, void *page, int prot);

__must_check
int mm_map_segment(struct mm_struct *mm, uintptr_t start, uintptr_t end, int prot);

__must_check
int mm_map_file_segment(struct mm_struct *mm, struct file *file,
				     uintptr_t start, uintptr_t end, int prot,
				     uint64_t file_offset);

__must_check
int mm_add_stack(struct mm_struct *mm, const void *stack, size_t stack_size);

__must_check
int mm_finalize(struct mm_struct *mm, uintptr_t first_vaddr, uintptr_t last_end);

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
 * @param flags Linux MAP_* bits accepted by cuteOS.
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
 * @param flags Linux MAP_* bits accepted by cuteOS.
 * @param fd File descriptor resolved by the syscall layer.
 * @param offset File offset in bytes; must satisfy page-alignment rules.
 * @return Mapped user address, or a negative errno.
 */
__must_check
ssize_t mm_mmap_file(struct mm_struct *mm, uintptr_t addr, size_t length, int prot, int flags,
		int fd, uint64_t offset);

/**
 * @brief Remove mappings from a user address range.
 * @param mm Address space to update.
 * @param addr Page-aligned start address.
 * @param length Range length in bytes.
 * @return 0 on success, or a negative errno.
 */
__must_check
int mm_munmap(struct mm_struct *mm, uintptr_t addr, size_t length);

__must_check
int mm_madvise(struct mm_struct *mm, uintptr_t addr, size_t len, int advice);

__must_check __nonnull(1)
int mm_mlock(struct mm_struct *mm, uintptr_t addr, size_t len);

 __must_check __nonnull(1)
int mm_munlock(struct mm_struct *mm, uintptr_t addr, size_t len);

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

__must_check
ssize_t mm_mremap(struct mm_struct *mm, uintptr_t old_addr, size_t old_size, size_t new_size,
		int flags, uintptr_t new_addr);

__must_check
int mm_msync(struct mm_struct *mm, uintptr_t addr, size_t len, int flags);

/**
 * @brief Validate that a user pointer range is inside user virtual memory.
 * @param addr User pointer start.
 * @param size Number of bytes in the range.
 * @return true when the range is a valid user address interval.
 */
__must_check
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

/**
 * @brief Copy bytes from kernel memory to userspace.
 * @param to Destination user pointer.
 * @param from Source kernel pointer.
 * @param n Number of bytes requested.
 * @return Number of bytes not copied; 0 means complete success.
 *
 * User memory must cross the kernel/userspace boundary through this helper or
 * an equivalent uaccess helper, never through direct dereference.
 */
__must_check  __access(write_only, 1, 3) __access(read_only, 2, 3)
size_t copy_to_user(void *to, const void *from, size_t n);


/**
 * @brief Copy bytes from userspace to kernel memory.
 * @param to Destination kernel pointer.
 * @param from Source user pointer.
 * @param n Number of bytes requested.
 * @return Number of bytes not copied; 0 means complete success.
 */
__must_check __access(write_only, 1, 3) __access(read_only, 2, 3)
size_t copy_from_user(void *to, const void *from, size_t n);


/**
 * @brief Copy a NUL-terminated string from userspace.
 * @param dst Kernel destination buffer.
 * @param src User source pointer.
 * @param maxlen Maximum bytes to copy, including the terminator.
 * @return String length excluding NUL, or a negative errno.
 */
__must_check __access(read_only, 2, 3) __access(write_only, 1, 3)
ssize_t strncpy_from_user(char *dst, const char *src, size_t maxlen);

/**
 * @brief Resolve a user instruction/load/store page fault.
 * @param tf Trap frame holding faulting user context and scause/stval state.
 */
__nonnull(1)
void do_page_fault(struct trap_frame *tf);

#endif
