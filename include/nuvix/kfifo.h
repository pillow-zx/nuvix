/**
 * @file kfifo.h
 * @brief Fixed-size, caller-owned FIFO for fixed-size kernel objects.
 *
 * A kfifo copies objects into caller-supplied storage and performs no memory
 * allocation or synchronization.  The storage and descriptor must outlive
 * every operation.  Callers must serialize concurrent access according to
 * their subsystem's locking and IRQ rules.
 */

#ifndef _NUVIX_KFIFO_H
#define _NUVIX_KFIFO_H

#include <nuvix/types.h>
#include <nuvix/compiler.h>
#include <nuvix/errno.h>

/**
 * @struct kfifo
 * @brief Circular FIFO descriptor over fixed-size object storage.
 *
 * The descriptor does not own @c storage.  All objects have @c elem_size
 * bytes.  Input and output buffers passed to operations must not overlap the
 * FIFO storage.
 */
struct kfifo {
	void *storage;
	size_t elem_size;
	size_t capacity;
	size_t head;
	size_t tail;
	size_t count;
};

/**
 * @def KFIFO_INIT
 * @brief Static initializer for a kfifo over caller-provided storage.
 */
#define KFIFO_INIT(buffer_ptr, element_bytes, element_count)                   \
	{                                                                      \
		.storage = (buffer_ptr),                                       \
		.elem_size = (element_bytes),                                  \
		.capacity = (element_count),                                   \
		.head = 0,                                                     \
		.tail = 0,                                                     \
		.count = 0,                                                    \
	}

/**
 * @def KFIFO_DECLARE
 * @brief Declare object storage and an automatic kfifo descriptor.
 */
#define KFIFO_DECLARE(name, type, nr_elements)                                 \
	static_assert((nr_elements) > 0,                                       \
		      "KFIFO_DECLARE requires non-zero capacity");             \
	type name##_storage[(nr_elements)];                                    \
	struct kfifo name =                                                    \
		KFIFO_INIT(name##_storage, sizeof(type), (nr_elements))

/**
 * @def KFIFO_DECLARE_STATIC
 * @brief Declare static object storage and a static kfifo descriptor.
 */
#define KFIFO_DECLARE_STATIC(name, type, nr_elements)                          \
	static_assert((nr_elements) > 0,                                       \
		      "KFIFO_DECLARE_STATIC requires non-zero capacity");      \
	static type name##_storage[(nr_elements)];                             \
	static struct kfifo name =                                             \
		KFIFO_INIT(name##_storage, sizeof(type), (nr_elements))

__must_check __pure
static inline bool kfifo_config_valid(const void *storage, size_t elem_size, size_t capacity)
{
	size_t storage_bytes;

	return storage != NULL && elem_size != 0 && capacity != 0 &&
	       !check_mul_overflow(elem_size, capacity, &storage_bytes);
}

/**
 * @brief Return whether a kfifo descriptor satisfies its invariants.
 */
__must_check __pure
static inline bool kfifo_valid(const struct kfifo *fifo)
{
	return fifo != NULL &&
	       kfifo_config_valid(fifo->storage, fifo->elem_size,
				  fifo->capacity) &&
	       fifo->head < fifo->capacity && fifo->tail < fifo->capacity &&
	       fifo->count <= fifo->capacity;
}

/**
 * @brief Initialize a kfifo over external fixed-size object storage.
 * @return 0 on success or -EINVAL for an invalid descriptor or configuration.
 */
__must_check
static inline int kfifo_init(struct kfifo *fifo, void *storage, size_t elem_size, size_t capacity)
{
	if (fifo == NULL)
		return -EINVAL;

	*fifo = (struct kfifo){};
	if (!kfifo_config_valid(storage, elem_size, capacity))
		return -EINVAL;

	fifo->storage = storage;
	fifo->elem_size = elem_size;
	fifo->capacity = capacity;
	return 0;
}

__must_check __pure
static inline bool kfifo_empty(const struct kfifo *fifo)
{
	return fifo->count == 0;
}

__must_check __pure
static inline bool kfifo_full(const struct kfifo *fifo)
{
	return fifo->count == fifo->capacity;
}

__must_check __pure
static inline size_t kfifo_size(const struct kfifo *fifo)
{
	return fifo->count;
}

__must_check __pure
static inline size_t kfifo_capacity(const struct kfifo *fifo)
{
	return fifo->capacity;
}

/**
 * @brief Discard all objects while retaining the storage binding.
 */
__must_check
static inline int kfifo_reset(struct kfifo *fifo)
{
	if (!kfifo_valid(fifo))
		return -EINVAL;

	fifo->head = 0;
	fifo->tail = 0;
	fifo->count = 0;
	return 0;
}

static inline void *kfifo_slot(const struct kfifo *fifo, size_t index)
{
	return (char *)fifo->storage + index * fifo->elem_size;
}

static inline size_t kfifo_next(const struct kfifo *fifo, size_t index)
{
	return index == fifo->capacity - 1 ? 0 : index + 1;
}

/**
 * @brief Copy one object onto the FIFO tail.
 * @return 0, -EINVAL, or -ENOSPC when the FIFO is full.
 */
__must_check
static inline int kfifo_put(struct kfifo *fifo, const void *element)
{
	if (!kfifo_valid(fifo) || element == NULL)
		return -EINVAL;
	if (kfifo_full(fifo))
		return -ENOSPC;

	memcpy(kfifo_slot(fifo, fifo->head), element, fifo->elem_size);
	fifo->head = kfifo_next(fifo, fifo->head);
	fifo->count++;
	return 0;
}

/**
 * @brief Copy and remove one object from the FIFO head.
 * @return 0, -EINVAL, or -ENODATA when the FIFO is empty.
 */
__must_check
static inline int kfifo_get(struct kfifo *fifo, void *element)
{
	if (!kfifo_valid(fifo) || element == NULL)
		return -EINVAL;
	if (kfifo_empty(fifo))
		return -ENODATA;

	memcpy(element, kfifo_slot(fifo, fifo->tail), fifo->elem_size);
	fifo->tail = kfifo_next(fifo, fifo->tail);
	fifo->count--;
	return 0;
}

/**
 * @brief Copy, without removing, the object at the FIFO head.
 * @return 0, -EINVAL, or -ENODATA when the FIFO is empty.
 */
__must_check
static inline int kfifo_peek(const struct kfifo *fifo, void *element)
{
	if (!kfifo_valid(fifo) || element == NULL)
		return -EINVAL;
	if (kfifo_empty(fifo))
		return -ENODATA;

	memcpy(element, kfifo_slot(fifo, fifo->tail), fifo->elem_size);
	return 0;
}

/**
 * @brief Copy up to @p nr_elements objects into the FIFO.
 * @return Number of objects copied; zero for no space or invalid arguments.
 */
__must_check
static inline size_t kfifo_in(struct kfifo *fifo, const void *elements, size_t nr_elements)
{
	const char *source = elements;
	size_t nr_copy;

	if (!kfifo_valid(fifo) || (nr_elements != 0 && elements == NULL))
		return 0;

	nr_copy = nr_elements;
	if (nr_copy > fifo->capacity - fifo->count)
		nr_copy = fifo->capacity - fifo->count;

	for (size_t i = 0; i < nr_copy; i++) {
		memcpy(kfifo_slot(fifo, fifo->head),
		       source + i * fifo->elem_size, fifo->elem_size);
		fifo->head = kfifo_next(fifo, fifo->head);
	}

	fifo->count += nr_copy;
	return nr_copy;
}

/**
 * @brief Copy and remove up to @p nr_elements objects from the FIFO.
 * @return Number of objects copied; zero for an empty FIFO or invalid input.
 */
__must_check
static inline size_t kfifo_out(struct kfifo *fifo, void *elements, size_t nr_elements)
{
	char *destination = elements;
	size_t nr_copy;

	if (!kfifo_valid(fifo) || (nr_elements != 0 && elements == NULL))
		return 0;

	nr_copy = nr_elements;
	if (nr_copy > fifo->count)
		nr_copy = fifo->count;

	for (size_t i = 0; i < nr_copy; i++) {
		memcpy(destination + i * fifo->elem_size,
		       kfifo_slot(fifo, fifo->tail), fifo->elem_size);
		fifo->tail = kfifo_next(fifo, fifo->tail);
	}

	fifo->count -= nr_copy;
	return nr_copy;
}

#endif
