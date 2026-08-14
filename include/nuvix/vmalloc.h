#ifndef _NUVIX_VMALLOC_H
#define _NUVIX_VMALLOC_H

#include <nuvix/alloc.h>
#include <nuvix/compiler.h>
#include <nuvix/types.h>

void vmalloc_init(void);

__must_check __malloc __alloc_size(1)
void *vmalloc(size_t size, enum alloc_mode mode);

__nonnull(1)
void vfree(void *ptr);

#endif
