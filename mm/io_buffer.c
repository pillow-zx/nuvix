#include <nuvix/processor.h>
#include <nuvix/io_buffer.h>
#include <nuvix/buddy.h>
#include <nuvix/errno.h>
#include <nuvix/slab.h>
#include "internal.h"

#define IO_PIN_LIMIT	4096
#define IO_BUFFER_PAGES 256
static atomic_t pinned_pages = ATOMIC_INIT(0);

void io_buffer_get(struct io_buffer *buffer)
{
	refcount_inc(&buffer->refs);
}

void io_buffer_put(struct io_buffer *buffer)
{
	if (!buffer || !refcount_dec_and_test(&buffer->refs))
		return;
	for (size_t i = 0; i < buffer->nr_pages; i++) {
		if (buffer->writable)
			atomic_fetch_sub_order(&buffer->pages[i]->io_write_pins,
					       1, ATOMIC_ORDER_SEQ_CST);
		page_put(buffer->pages[i]);
	}
	atomic_fetch_sub_order(&pinned_pages, buffer->nr_pages,
			       ATOMIC_ORDER_SEQ_CST);
	mm_put(buffer->mm);
	kfree(buffer);
}

int io_buffer_pin(struct mm_struct *mm, uintptr_t address, size_t length,
		  bool writable, struct io_buffer **out)
{
	struct io_buffer *buffer;
	struct mm_teardown teardown = {0};
	size_t count;
	int ret = 0;

	*out = NULL;
	if (!mm || !access_ok((void *)address, length))
		return -EFAULT;
	count = length ? (ALIGN_UP((address & (PAGE_SIZE - 1)) + length, PAGE_SIZE) >> PAGE_SHIFT) : 0;
	if (count > IO_BUFFER_PAGES)
		return -E2BIG;
	if (atomic_fetch_add_order(&pinned_pages, count, ATOMIC_ORDER_SEQ_CST) + count > IO_PIN_LIMIT) {
		atomic_fetch_sub_order(&pinned_pages, count, ATOMIC_ORDER_SEQ_CST);
		return -ENOMEM;
	}
	buffer = kmalloc(sizeof(*buffer) + count * sizeof(struct page *),
			 ALLOC_NOWAIT);
	if (!buffer) {
		atomic_fetch_sub_order(&pinned_pages, count, ATOMIC_ORDER_SEQ_CST);
		return -ENOMEM;
	}
	refcount_set(&buffer->refs, 1);
	buffer->mm = mm;
	buffer->address = address;
	buffer->length = length;
	buffer->writable = writable;
	buffer->nr_pages = 0;
	mm_get(mm);
	mm_lock(mm);
	for (size_t i = 0; i < count; i++) {
		uintptr_t va = (address & PAGE_MASK) + i * PAGE_SIZE;
		struct vm_area_struct *vma = find_vma(mm, va);
		pte_t *pte;
		/* File-backed pages and kernel-owned ring storage cannot be
		 * pinned. */
		if (!vma || vma->vm_file ||
		    (vma->vm_anon && vma->vm_anon->kernel_page)) {
			ret = -EFAULT;
			break;
		}
		ret = fault_in_urange_locked(mm, va, PAGE_SIZE,
				writable ? USER_FAULT_WRITE : USER_FAULT_READ, &teardown);
		if (ret < 0)
			break;
		/* Fault preparation may drop mmap_lock. */
		vma = find_vma(mm, va);
		pte = pt_lookup(mm->pgroot, va);
		if (!vma || vma->vm_file ||
		    (vma->vm_anon && vma->vm_anon->kernel_page) || !pte ||
		    (writable ? !pte_uwrite(*pte) : !pte_uread(*pte))) {
			ret = -EFAULT;
			break;
		}
		buffer->pages[i] = virt_to_page(__va(pte_phys(*pte)));
		page_get(buffer->pages[i]);
		if (writable)
			atomic_fetch_add_order(&buffer->pages[i]->io_write_pins,
					1, ATOMIC_ORDER_SEQ_CST);
		buffer->nr_pages++;
	}
	mm_unlock(mm);
	mm_teardown_release(&teardown);
	if (ret < 0) {
		atomic_fetch_sub_order(&pinned_pages, count - buffer->nr_pages,
				       ATOMIC_ORDER_SEQ_CST);
		io_buffer_put(buffer);
		return ret;
	}
	*out = buffer;
	return 0;
}

void io_buffer_copy(struct io_buffer *buffer, size_t offset, void *kernel,
		    size_t length, bool to_user)
{
	size_t position = (buffer->address & (PAGE_SIZE - 1)) + offset;
	BUG_ON(offset > buffer->length || length > buffer->length - offset);
	BUG_ON(to_user && !buffer->writable);
	/* Serialize copies against fork's private snapshot. No faults or VFS
	 * locks. */
	mm_lock(buffer->mm);
	while (length) {
		size_t part = MIN(length, PAGE_SIZE - (position & (PAGE_SIZE - 1)));
		char *data = page_to_virt(buffer->pages[position >> PAGE_SHIFT]);
		data += position & (PAGE_SIZE - 1);
		if (to_user)
			memcpy(data, kernel, part);
		else
			memcpy(kernel, data, part);
		kernel = (char *)kernel + part;
		position += part;
		length -= part;
	}
	if (to_user) {
		flush_icache();
		mm_flush_remote(buffer->mm, true);
	}
	mm_unlock(buffer->mm);
}
