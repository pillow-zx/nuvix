#ifndef _CUTEOS_KERNEL_VMALLOC_H
#define _CUTEOS_KERNEL_VMALLOC_H

#include <kernel/alloc.h>
#include <kernel/compiler.h>
#include <kernel/types.h>

void vmalloc_init(void);

__must_check __malloc __alloc_size(1)
void *vmalloc(size_t size, enum alloc_mode mode);

__nonnull(1)
void vfree(void *ptr);

#endif
