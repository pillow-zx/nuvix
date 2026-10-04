#include <nuvix/memblock.h>
#include <nuvix/dtb.h>
#include <nuvix/errno.h>
#include <nuvix/layout.h>
#include <nuvix/math.h>
#include <nuvix/string.h>

extern char __start[], __end[];

struct memblock memblock;

int memblock_reserve(uint64_t base, uint64_t size, bool no_map)
{
    struct memblock_region *regions = memblock.reserved;
    unsigned int merged = 0, count = 0, pos = 0;
    uint64_t end;

    if (!size)
        return -EINVAL;
    if (size > UINT64_MAX - base)
        return -EOVERFLOW;
    end = base + size;

    for (unsigned int i = 0; i < memblock.nr_reserved; ++i) {
        const struct memblock_region *region = &regions[i];
        uint64_t region_end = region->base + region->size;

        if (region->no_map != no_map || region->base > end || region_end < base)
            continue;
        base = MIN(base, region->base);
        end = MAX(end, region_end);
        ++merged;
    }
    if (memblock.nr_reserved - merged == MEMBLOCK_MAX_RESERVED)
        return -ENOSPC;

    for (unsigned int i = 0; i < memblock.nr_reserved; ++i) {
        struct memblock_region region = regions[i];

        if (region.no_map == no_map && region.base <= end &&
            region.base + region.size >= base)
            continue;
        regions[count++] = region;
    }
    while (pos < count && regions[pos].base < base)
        ++pos;
    memmove(&regions[pos + 1], &regions[pos], (count - pos) * sizeof(*regions));
    regions[pos] = (struct memblock_region){
        .base = base, .size = end - base, .no_map = no_map};
    memblock.nr_reserved = count + 1;
    return 0;
}

int early_alloc(size_t size, size_t align, paddr_t *paddr)
{
    uint64_t base, limit;
    int ret;

    if (!size || !IS_POWER_OF_2(align) || !paddr)
        return -EINVAL;
    if (size > SIZE_MAX - (PAGE_SIZE - 1))
        return -EOVERFLOW;
    size = ALIGN_UP(size, (size_t)PAGE_SIZE);
    align = MAX(align, (size_t)PAGE_SIZE);
    base = memblock.memory.base;
    limit = base + memblock.memory.size;

    /* Reserved regions are sorted by base, but may overlap. */
    for (unsigned int i = 0;; ++i) {
        const struct memblock_region *region;
        uint64_t region_end;

        if (base >= limit || align - 1 > UINT64_MAX - base)
            return -ENOMEM;
        base = ALIGN_UP(base, (uint64_t)align);
        if (base >= limit || size > limit - base)
            return -ENOMEM;
        if (i == memblock.nr_reserved)
            break;

        region = &memblock.reserved[i];
        region_end = region->base + region->size;
        if (region_end <= base)
            continue;
        if (region->base >= base + size)
            break;
        base = region_end;
    }

    ret = memblock_reserve(base, size, false);
    if (ret)
        return ret;
    *paddr = base;
    return 0;
}

static int register_memory(const struct dtb_range *range, void *arg)
{
    (void)arg;
    if (memblock.memory.size)
        return -EOPNOTSUPP;
    memblock.memory.base = range->base;
    memblock.memory.size = range->size;
    return 0;
}

static int register_reserved(const struct dtb_range *range, void *arg)
{
    (void)arg;
    return memblock_reserve(range->base, range->size, range->no_map);
}

int memblock_init(const struct dtb *dtb)
{
    uint64_t ksize = (uintptr_t)__end - (uintptr_t)__start;
    int ret;

    memblock = (struct memblock){0};
    ret = dtb_scan_memory(dtb, register_memory, NULL);
    if (ret)
        return ret;
    ret = dtb_scan_reserved(dtb, register_reserved, NULL);
    if (ret)
        return ret;
    ret = memblock_reserve(dtb->paddr, dtb->size, false);
    if (ret)
        return ret;
    return memblock_reserve(KERNEL_PBASE, ksize, false);
}
