#ifndef _NUVIX_BOOTMEM_H
#define _NUVIX_BOOTMEM_H

#include <nuvix/types.h>

extern paddr_t ram_base;
extern size_t ram_size;
void bootmem_init(void);
void bootmem_reserve(paddr_t start, size_t size, bool no_map);
bool bootmem_reserved(paddr_t start, size_t size);
bool bootmem_no_map(paddr_t start, size_t size);
/* Zeroed page-aligned allocation. Only available before buddy_init(). */
void *bootmem_alloc(size_t size);
void bootmem_mapped(void);
void bootmem_finish(void);

#endif
