#include <arch/io.h>

#include <arch/pgtable.h>
#include <nuvix/bootmem.h>
#include <nuvix/printk.h>
#include <nuvix/smp.h>
#include <nuvix/tools.h>

static vaddr_t next_mmio = MMIO_VBASE;

vaddr_t mmio_map(paddr_t address, size_t size)
{
	size_t offset = address & (PAGE_SIZE - 1);
	paddr_t start = address & PAGE_MASK;
	if (smp_booted() || !size || address >= (1UL << 56) ||
	    size > (1UL << 56) - address || size > MMIO_SIZE - offset)
		panic("mmio: invalid boot mapping");
	size_t span = ALIGN_UP(offset + size, PAGE_SIZE);
	if (span > MMIO_VBASE + MMIO_SIZE - next_mmio ||
	    (start < ram_base + ram_size && ram_base < start + span))
		panic("mmio: exhausted window or resource overlaps RAM");
	vaddr_t result = next_mmio + offset;
	for (size_t i = 0; i < span; i += PAGE_SIZE)
		if (map_page(kpgtable(), next_mmio + i, start + i, PTE_KERN_RW))
			panic("mmio: page table allocation failed");
	next_mmio += span;
	tlb_flush_all();
	return result;
}
