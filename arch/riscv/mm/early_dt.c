#include <arch/boot.h>
#include <arch/pgtable.h>
#include <nuvix/dt.h>
#include <nuvix/printk.h>

extern pte_t tmp_root[];
/* A page offset can make a 16 MiB blob span nine 2 MiB table spans. */
static pte_t dt_l1[512] __aligned(PAGE_SIZE);
static pte_t dt_l0[9][512] __aligned(PAGE_SIZE);

static const void *dt_map(paddr_t pa, size_t size)
{
	paddr_t page = pa & PAGE_MASK;
	size_t offset = pa - page;
	size_t pages = (offset + size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	for (size_t i = 0; i < pages; i++) {
		dt_l0[i / 512][i % 512] = PA_TO_PTE(page + i * PAGE_SIZE) | PTE_KERN_R;
		dt_l1[i / 512] = PA_TO_PTE(__pa(dt_l0[i / 512])) | PTE_TABLE;
	}
	tmp_root[(DTB_VBASE >> 30) & 511] = PA_TO_PTE(__pa(dt_l1)) | PTE_TABLE;
	tlb_flush_all();
	return (const void *)(DTB_VBASE + offset);
}

void arch_dtb_init(paddr_t pa)
{
	if (!pa || (pa & 7) || pa >= (1UL << 56) - DTB_MAX_SIZE)
		panic("dt: invalid physical address 0x%lx", pa);
	const void *blob = dt_map(pa, sizeof(struct fdt_header));
	int ret = fdt_check_header(blob);
	if (ret)
		panic("dt: invalid header: %s", fdt_strerror(ret));
	size_t size = fdt_totalsize(blob);
	if (size < sizeof(struct fdt_header) || size > DTB_MAX_SIZE)
		panic("dt: unsupported blob size %lu", size);
	blob = dt_map(pa, size);
	dt_init(blob, size);
}
