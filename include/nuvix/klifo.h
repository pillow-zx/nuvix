/**
 * @file klifo.h
 * @brief Fixed-size, caller-owned LIFO for fixed-size kernel objects.
 *
 * A klifo copies objects into caller-supplied storage and performs no memory
 * allocation or synchronization.  The storage and descriptor must outlive
 * every operation.  Callers must serialize concurrent access according to
 * their subsystem's locking and IRQ rules.
 */

#ifndef _NUVIX_KLIFO_H
#define _NUVIX_KLIFO_H

#include <nuvix/types.h>
#include <nuvix/compiler.h>
#include <nuvix/errno.h>

/**
 * @struct klifo
 * @brief LIFO descriptor over fixed-size object storage.
 *
 * The descriptor does not own @c storage.  All objects have @c elem_size
 * bytes.  Input and output buffers passed to operations must not overlap the
 * LIFO storage.
 */
struct klifo {
	void *storage;
	size_t elem_size;
	size_t capacity;
	size_t count;
};

/**
 * @def KLIFO_INIT
 * @brief Static initializer for a klifo over caller-provided storage.
 */
#define KLIFO_INIT(buffer_ptr, element_bytes, element_count)                   \
	{                                                                      \
		.storage = (buffer_ptr),                                       \
		.elem_size = (element_bytes),                                  \
		.capacity = (element_count),                                   \
		.count = 0,                                                    \
	}

/**
 * @def KLIFO_DECLARE
 * @brief Declare object storage and an automatic klifo descriptor.
 */
#define KLIFO_DECLARE(name, type, nr_elements)                                 \
	static_assert((nr_elements) > 0,                                       \
		      "KLIFO_DECLARE requires non-zero capacity");             \
	type name##_storage[(nr_elements)];                                    \
	struct klifo name =                                                    \
		KLIFO_INIT(name##_storage, sizeof(type), (nr_elements))

/**
 * @def KLIFO_DECLARE_STATIC
 * @brief Declare static object storage and a static klifo descriptor.
 */
#define KLIFO_DECLARE_STATIC(name, type, nr_elements)                          \
	static_assert((nr_elements) > 0,                                       \
		      "KLIFO_DECLARE_STATIC requires non-zero capacity");      \
	static type name##_storage[(nr_elements)];                             \
	static struct klifo name =                                             \
		KLIFO_INIT(name##_storage, sizeof(type), (nr_elements))

__must_check __pure
static inline bool klifo_config_valid(const void *storage, size_t elem_size, size_t capacity)
{
	size_t storage_bytes;

	return storage != NULL && elem_size != 0 && capacity != 0 &&
	       !check_mul_overflow(elem_size, capacity, &storage_bytes);
}

/**
 * @brief Return whether a klifo descriptor satisfies its invariants.
 */
__must_check __pure
static inline bool klifo_valid(const struct klifo *lifo)
{
	return lifo != NULL &&
	       klifo_config_valid(lifo->storage, lifo->elem_size,
				  lifo->capacity) &&
	       lifo->count <= lifo->capacity;
}

/**
 * @brief Initialize a klifo over external fixed-size object storage.
 * @return 0 on success or -EINVAL for an invalid descriptor or configuration.
 */
__must_check
static inline int klifo_init(struct klifo *lifo, void *storage, size_t elem_size, size_t capacity)
{
	if (lifo == NULL)
		return -EINVAL;

	*lifo = (struct klifo){};
	if (!klifo_config_valid(storage, elem_size, capacity))
		return -EINVAL;

	lifo->storage = storage;
	lifo->elem_size = elem_size;
	lifo->capacity = capacity;
	return 0;
}

__always_inline __must_check __pure __nonnull(1)
static inline bool klifo_empty(const struct klifo *lifo)
{
	return lifo->count == 0;
}

__always_inline __must_check __pure __nonnull(1)
static inline bool klifo_full(const struct klifo *lifo)
{
	return lifo->count == lifo->capacity;
}

__always_inline __must_check __pure __nonnull(1)
static inline size_t klifo_size(const struct klifo *lifo)
{
	return lifo->count;
}

__always_inline __must_check __pure __nonnull(1)
static inline size_t klifo_capacity(const struct klifo *lifo)
{
	return lifo->capacity;
}

/**
 * @brief Discard all objects while retaining the storage binding.
 */
__must_check
static inline int klifo_reset(struct klifo *lifo)
{
	if (!klifo_valid(lifo))
		return -EINVAL;

	lifo->count = 0;
	return 0;
}

__always_inline __pure __nonnull(1)
static inline void *klifo_slot(const struct klifo *lifo, size_t index)
{
	return (char *)lifo->storage + index * lifo->elem_size;
}

/**
 * @brief Copy one object onto the LIFO top.
 * @return 0, -EINVAL, or -ENOSPC when the LIFO is full.
 */
__must_check
static inline int klifo_push(struct klifo *lifo,
					  const void *element)
{
	if (!klifo_valid(lifo) || element == NULL)
		return -EINVAL;
	if (klifo_full(lifo))
		return -ENOSPC;

	memcpy(klifo_slot(lifo, lifo->count), element, lifo->elem_size);
	lifo->count++;
	return 0;
}

/**
 * @brief Copy and remove the object at the LIFO top.
 * @return 0, -EINVAL, or -ENODATA when the LIFO is empty.
 */
__must_check
static inline int klifo_pop(struct klifo *lifo, void *element)
{
	if (!klifo_valid(lifo) || element == NULL)
		return -EINVAL;
	if (klifo_empty(lifo))
		return -ENODATA;

	lifo->count--;
	memcpy(element, klifo_slot(lifo, lifo->count), lifo->elem_size);
	return 0;
}

/**
 * @brief Copy, without removing, the object at the LIFO top.
 * @return 0, -EINVAL, or -ENODATA when the LIFO is empty.
 */
__must_check
static inline int klifo_peek(const struct klifo *lifo,
					  void *element)
{
	if (!klifo_valid(lifo) || element == NULL)
		return -EINVAL;
	if (klifo_empty(lifo))
		return -ENODATA;

	memcpy(element, klifo_slot(lifo, lifo->count - 1), lifo->elem_size);
	return 0;
}

#endif
