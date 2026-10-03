#ifndef _NUVIX_MEMBLOCK_H
#define _NUVIX_MEMBLOCK_H

#include <nuvix/ctypes.h>

#define MEMBLOCK_MAX_RESERVED 32

struct dtb;

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

int memblock_init(const struct dtb *dtb);
int memblock_reserve(uint64_t base, uint64_t size, bool no_map);

#endif /* _NUVIX_MEMBLOCK_H */
