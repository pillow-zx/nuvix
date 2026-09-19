#include <nuvix/bootmem.h>
#include <nuvix/dt.h>
#include <nuvix/errno.h>
#include <nuvix/page.h>
#include <nuvix/printk.h>
#include <nuvix/tools.h>

#define MAX_RANGES 128

struct range { paddr_t start, end; };
struct ranges { struct range entries[MAX_RANGES]; unsigned count; };
static struct ranges reserved, unmapped, memory;
static paddr_t cursor, alloc_end;
static bool finished;
paddr_t ram_base;
size_t ram_size;
extern char _start[], _end[];

/* Sorted union; no-map is a separate union so overlapping reservations do
 * not accidentally extend no-map semantics to ordinary reserved pages. */
static void insert(struct ranges *set, paddr_t start, paddr_t end)
{
	unsigned i = 0;
	while (i < set->count && set->entries[i].end < start)
		i++;
	while (i < set->count && set->entries[i].start <= end) {
		if (set->entries[i].start < start)
			start = set->entries[i].start;
		if (set->entries[i].end > end)
			end = set->entries[i].end;
		memmove(&set->entries[i], &set->entries[i + 1],
			(set->count - i - 1) * sizeof(struct range));
		set->count--;
	}
	if (set->count == MAX_RANGES)
		panic("bootmem: too many memory ranges");
	memmove(&set->entries[i + 1], &set->entries[i],
		(set->count - i) * sizeof(struct range));
	set->entries[i] = (struct range){start, end};
	set->count++;
}

static bool overlaps(const struct ranges *set, paddr_t start, size_t size)
{
	for (unsigned i = 0; i < set->count; i++)
		if (start < set->entries[i].end && set->entries[i].start < start + size)
			return true;
	return false;
}

bool bootmem_reserved(paddr_t start, size_t size)
{
	return overlaps(&reserved, start, size);
}

bool bootmem_no_map(paddr_t start, size_t size)
{
	return overlaps(&unmapped, start, size);
}

void bootmem_reserve(paddr_t start, size_t size, bool no_map)
{
	paddr_t end;
	if (!size)
		return;
	if (finished || start > UINT64_MAX - size ||
	    start + size > UINT64_MAX - (PAGE_SIZE - 1))
		panic("bootmem: invalid reservation");
	end = ALIGN_UP(start + size, PAGE_SIZE);
	start &= PAGE_MASK;
	insert(&reserved, start, end);
	if (no_map)
		insert(&unmapped, start, end);
}

void bootmem_init(void)
{
	int node = -1, ret;
	struct dt_resource resource;
	while ((node = fdt_next_node(dt_blob, node, NULL)) >= 0) {
		const char *type = dt_string(node, "device_type");
		if (!type || strcmp(type, "memory") || !dt_available(node))
			continue;
		unsigned index = 0;
		while (!(ret = dt_reg(node, index++, &resource))) {
			if (resource.start + resource.size > DIRECT_MAP_LIMIT)
				panic("bootmem: RAM exceeds direct-map capacity");
			paddr_t start = ALIGN_UP(resource.start, PAGE_SIZE);
			paddr_t end = (resource.start + resource.size) & PAGE_MASK;
			if (start < end)
				insert(&memory, start, end);
		}
		if (ret != -ENOENT || index == 1)
			panic("bootmem: invalid memory reg");
	}
	if (memory.count != 1)
		panic("bootmem: expected contiguous RAM, found %u ranges", memory.count);
	ram_base = memory.entries[0].start;
	ram_size = memory.entries[0].end - ram_base;
	if (__pa(_start) < ram_base || __pa(_end) > ram_base + ram_size)
		panic("bootmem: kernel is outside RAM");
	/* Keep the firmware/load prefix reserved, even if firmware omitted it. */
	bootmem_reserve(ram_base, __pa(_end) - ram_base, false);
	for (int i = 0, n = fdt_num_mem_rsv(dt_blob); i < n; i++) {
		uint64_t start, size;
		if (fdt_get_mem_rsv(dt_blob, i, &start, &size))
			panic("bootmem: malformed reservation map");
		bootmem_reserve(start, size, false);
	}
	int parent = fdt_path_offset(dt_blob, "/reserved-memory");
	if (parent >= 0) {
		fdt_for_each_subnode(node, dt_blob, parent) {
			if (!dt_available(node))
				continue;
			unsigned index = 0;
			bool no_map = fdt_getprop(dt_blob, node, "no-map", NULL) != NULL;
			while (!(ret = dt_reg(node, index++, &resource)))
				bootmem_reserve(resource.start, resource.size, no_map);
			if (ret != -ENOENT || index == 1)
				panic("bootmem: unsupported reserved-memory node %s",
				      fdt_get_name(dt_blob, node, NULL));
		}
	}
	if (bootmem_no_map(__pa(_start), __pa(_end) - __pa(_start)))
		panic("bootmem: kernel overlaps no-map memory");
	cursor = ALIGN_UP(__pa(_end), PAGE_SIZE);
	alloc_end = ram_base + ram_size;
	if (alloc_end > BOOT_RAM_END)
		alloc_end = BOOT_RAM_END;
}

void *bootmem_alloc(size_t size)
{
	if (finished || !size || size > SIZE_MAX - PAGE_SIZE + 1)
		panic("bootmem: invalid allocation");
	size = ALIGN_UP(size, PAGE_SIZE);
	for (unsigned i = 0; i < reserved.count; i++) {
		struct range r = reserved.entries[i];
		if (cursor >= r.end)
			continue;
		if (cursor <= r.start && size <= r.start - cursor)
			break;
		cursor = r.end;
	}
	if (cursor > alloc_end || size > alloc_end - cursor)
		panic("bootmem: no accessible memory for %lu bytes", size);
	paddr_t start = cursor;
	cursor += size;
	bootmem_reserve(start, size, false);
	void *ptr = __va(start);
	memset(ptr, 0, size);
	return ptr;
}

void bootmem_mapped(void)
{
	alloc_end = ram_base + ram_size;
}

void bootmem_finish(void)
{
	finished = true;
}
