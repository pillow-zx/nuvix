#ifndef _NUVIX_MEMBLOCK_H
#define _NUVIX_MEMBLOCK_H

#include <nuvix/ctypes.h>

#define MEMBLOCK_MAX_RESERVED 32

struct memblock_region {
    uint64_t base;
    uint64_t size;
    bool no_map;
};

struct memblock {
    struct memblock_region memory;
    struct memblock_region reserved[MEMBLOCK_MAX_RESERVED];
    unsigned int nr_reserved;
};

extern struct memblock memblock;

int memblock_init(void);
int memblock_reserve(uint64_t base, uint64_t size, bool no_map);

/*
 * Boot-only allocation after memblock_init(), before buddy takes ownership.
 * Search RAM in memblock.memory, skipping reserved regions.
 * Size is rounded up to pages; align must be a nonzero power of two and is
 * raised to PAGE_SIZE if smaller. Memory is reserved, not zeroed, and has no
 * early free operation. Return 0 and a physical address on success, or a
 * negative errno without changing *phys_addr on failure.
 */
int early_alloc(size_t size, size_t align, paddr_t *paddr);

#endif /* _NUVIX_MEMBLOCK_H */
