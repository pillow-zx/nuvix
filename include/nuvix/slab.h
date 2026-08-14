/**
 * @file slab.h
 * @brief 小对象 slab 分配器接口。
 */

#ifndef _NUVIX_SLAB_H
#define _NUVIX_SLAB_H

#include <nuvix/alloc.h>
#include <nuvix/types.h>
#include <nuvix/compiler.h>
#include <nuvix/cleanup.h>

/**
 * @brief Initialize slab caches used by kmalloc.
 */
void slab_init(void);

/**
 * @brief Allocate a small kernel heap object.
 * @param size Requested object size in bytes.
 * @param mode Context permission for the allocation.
 * @return Allocated object, or NULL.
 */
__must_check __malloc __alloc_size(1)
void *kmalloc(size_t size, enum alloc_mode mode);

/**
 * @brief Allocate n * size space
 * @param n number of requirement
 * @param size per size of requirement obj
 * @param mode Context permission for the allocation.
 * @return Allocated objects, or NULL
 */
__must_check
static inline void *kmalloc_array(size_t n, size_t size,
				  enum alloc_mode mode)
{
	size_t bytes;

	if (check_mul_overflow(n, size, &bytes))
		return NULL;

	bytes = n * size;
	return kmalloc(bytes, mode);
}

/**
 * @brief Free an object allocated by kmalloc/kzalloc.
 * @param ptr Object pointer, or NULL.
 */
void kfree(void *ptr);

CLEANUP_DEFINE(kfree, void *, if (_T) kfree(_T));

/**
 * @brief Allocate and zero a small kernel heap object.
 * @param size Requested object size in bytes.
 * @param mode Context permission for the allocation.
 * @return Zero-filled object, or NULL.
 */
__must_check __malloc __alloc_size(1)
static inline void *kzalloc(size_t size, enum alloc_mode mode)
{
	void *ptr = kmalloc(size, mode);
	if (ptr)
		memset(ptr, 0, size);

	return ptr;
}

#endif
