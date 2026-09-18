#ifndef _NUVIX_IO_BUFFER_H
#define _NUVIX_IO_BUFFER_H

#include <nuvix/mm.h>
#include <nuvix/refcount.h>

/* Private to the asynchronous I/O core. Never a DMA/physical-address API.
 * Pin/unpin and copying run in sleepable context. Each buffer owns its mm
 * and page references; munmap cannot retarget an accepted operation. */
struct io_buffer {
	refcount_t refs;
	struct mm_struct *mm;
	uintptr_t address;
	size_t length;
	size_t nr_pages;
	bool writable;
	struct page *pages[];
};

__must_check
int io_buffer_pin(struct mm_struct *mm, uintptr_t address, size_t length, bool writable, struct io_buffer **out);

void io_buffer_get(struct io_buffer *buffer);

void io_buffer_put(struct io_buffer *buffer);

__nonnull(1, 3)
void io_buffer_copy(struct io_buffer *buffer, size_t offset, void *kernel, size_t length, bool to_user);

#endif
