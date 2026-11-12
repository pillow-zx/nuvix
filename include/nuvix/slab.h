#ifndef _NUVIX_SLAB_H
#define _NUVIX_SLAB_H

#include <nuvix/ctypes.h>
#include <nuvix/cleanup.h>

/* No reclaim or sleeping. Zero size and allocation failure return NULL. */
void *kmalloc(size_t size) __malloc __alloc_size(1);
void *kzalloc(size_t size) __malloc __alloc_size(1);
/* NULL is a no-op; otherwise pass the original allocation pointer. */
void kfree(const void *ptr);

DECLARE_FREE(kfree, void *, if (_T) kfree(_T))

#endif /* _NUVIX_SLAB_H */
