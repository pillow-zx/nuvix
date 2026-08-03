/*
 * arch/riscv/mm/page_table.c - Sv39 三级页表操作
 */

#include <kernel/printk.h>
#include <kernel/buddy.h>
#include <kernel/errno.h>
#include <kernel/tools.h>
#include <arch/page.h>
#include <arch/pgtable.h>
#include <asm/csr.h>

typedef void *(*page_alloc_fn)(void);
static page_alloc_fn pt_alloc;
static uintptr_t ksatp_val;

#ifdef KERNEL_SELFTEST
static int32_t pt_alloc_fail_after = -1;
#endif

static char *early_alloc_ptr;

void *bootmem_end(void)
{
	return early_alloc_ptr;
}

static void *early_alloc_page(void)
{
	void *p = early_alloc_ptr;
	early_alloc_ptr += PAGE_SIZE;
	memset(p, 0, PAGE_SIZE);
	return p;
}

static void *buddy_alloc_page(void)
{
	void *p = get_free_page(0, ALLOC_NOWAIT);
	if (p)
		memset(p, 0, PAGE_SIZE);
	return p;
}

static void *pt_alloc_page(void)
{
	BUG_ON(!pt_alloc);

#ifdef KERNEL_SELFTEST
	if (pt_alloc_fail_after == 0)
		return NULL;
	if (pt_alloc_fail_after > 0)
		pt_alloc_fail_after--;
#endif

	return pt_alloc();
}

#ifdef KERNEL_SELFTEST
void pagetable_test_fail_alloc_after(uint32_t successful_allocs)
{
	pt_alloc_fail_after = (int32_t)successful_allocs;
}

void pagetable_test_clear_alloc_failure(void)
{
	pt_alloc_fail_after = -1;
}
#endif

void pagetable_use_buddy(void)
{
	pt_alloc = buddy_alloc_page;
}

static bool pte_is_leaf(pte_t pte)
{
	return asm_pte_leaf(pte);
}

static int pt_walk_create(pte_t *root, vaddr_t va, pte_t **out)
{
	bool new_l1 = false;

	int idx2 = (va >> 30) & 0x1FF;
	pte_t *l2e = &root[idx2];
	pte_t *l1;

	if (!(*l2e & PTE_V)) {
		l1 = pt_alloc_page();
		if (!l1)
			return -ENOMEM;
		*l2e = PA_TO_PTE(__pa((uintptr_t)l1)) | PTE_TABLE;
		new_l1 = true;
	} else {
		if (pte_is_leaf(*l2e))
			return -EINVAL;
		l1 = (pte_t *)__va(PTE_TO_PA(*l2e));
	}

	int idx1 = (va >> 21) & 0x1FF;
	pte_t *l1e = &l1[idx1];
	pte_t *l0;

	if (!(*l1e & PTE_V)) {
		l0 = pt_alloc_page();
		if (!l0) {
			if (new_l1) {
				*l2e = 0;
				free_page(l1, 0);
			}
			return -ENOMEM;
		}
		*l1e = PA_TO_PTE(__pa((uintptr_t)l0)) | PTE_TABLE;
	} else {
		if (pte_is_leaf(*l1e))
			return -EINVAL;
		l0 = (pte_t *)__va(PTE_TO_PA(*l1e));
	}

	int idx0 = (va >> 12) & 0x1FF;
	*out = &l0[idx0];
	return 0;
}

pte_t *pagetable_lookup(pte_t *root, vaddr_t va)
{
	int idx2 = (va >> 30) & 0x1FF;
	pte_t *l2e = &root[idx2];

	if (!(*l2e & PTE_V))
		return NULL;
	if (pte_is_leaf(*l2e))
		return NULL;

	pte_t *l1 = (pte_t *)__va(PTE_TO_PA(*l2e));
	int idx1 = (va >> 21) & 0x1FF;
	pte_t *l1e = &l1[idx1];

	if (!(*l1e & PTE_V))
		return NULL;
	if (pte_is_leaf(*l1e))
		return NULL;

	pte_t *l0 = (pte_t *)__va(PTE_TO_PA(*l1e));
	int idx0 = (va >> 12) & 0x1FF;
	return &l0[idx0];
}

int map_page(pte_t *root, vaddr_t va, paddr_t pa, uint64_t perm)
{
	pte_t *pte;
	int ret;

	if (!IS_ALIGNED(va, PAGE_SIZE) || !IS_ALIGNED(pa, PAGE_SIZE))
		return -EINVAL;
	if (!(perm & PTE_V))
		return -EINVAL;

	ret = pt_walk_create(root, va, &pte);
	if (ret < 0)
		return ret;
	*pte = PA_TO_PTE(pa) | perm;
	return 0;
}

uintptr_t kernel_satp(void)
{
	return ksatp_val;
}

pte_t *current_pt(void)
{
	uintptr_t satp_val = csr_read(satp);
	uintptr_t root_pa = (satp_val & SATP_PPN_MASK) << PAGE_SHIFT;

	return (pte_t *)__va(root_pa);
}

pte_t *kernel_pt(void)
{
	uintptr_t satp_val = kernel_satp();
	uintptr_t root_pa = (satp_val & SATP_PPN_MASK) << PAGE_SHIFT;

	return (pte_t *)__va(root_pa);
}

pte_t *pagetable_lookup_current(uintptr_t va)
{
	return pagetable_lookup(current_pt(), va);
}

void pagetable_write_current(uintptr_t va, uintptr_t pa, pte_t perm)
{
	pte_t *pte = pagetable_lookup_current(va);

	if (!pte || !(*pte & PTE_V))
		panic("pt_write_current: no mapping for va=%p",
		      (void *)va);

	*pte = PA_TO_PTE(pa) | perm;
	tlb_flush_page(va);
}

void pagetable_init(void)
{
	extern char _end[];
	paddr_t end_addr;
	pte_t *root = NULL;

	end_addr = (paddr_t)_end;
	early_alloc_ptr = (char *)ALIGN_UP(end_addr, PAGE_SIZE);

	pt_alloc = early_alloc_page;

	root = (pte_t *)early_alloc_page();

	pr_info("page_table: mapping %dMB DRAM with 4KB pages...\n",
		(int)(DRAM_SIZE >> 20));

	for (paddr_t pa = DRAM_BASE; pa < DRAM_BASE + DRAM_SIZE;
	     pa += PAGE_SIZE) {
		vaddr_t va = KERNEL_VBASE + pa;
		BUG_ON(map_page(root, va, pa, PTE_KERN_RWX) < 0);
	}

	int idx_high = ((KERNEL_VBASE + DRAM_BASE) >> 30) & 0x1FF;
	int idx_id = (DRAM_BASE >> 30) & 0x1FF;
	root[idx_id] = root[idx_high];

	root[0] = PA_TO_PTE(0UL) | PTE_KERN_RW;

	paddr_t root_pa = __pa((uintptr_t)root);
	uintptr_t satp_val = SATP_MODE_SV39 | (root_pa >> PAGE_SHIFT);
	ksatp_val = satp_val;

	csr_write(satp, satp_val);
	tlb_flush_all();

	pr_info("page_table: switched to kernel page table (root=%p, "
		"early_alloc=%dKB)\n",
		(void *)root_pa,
		(int)((uintptr_t)early_alloc_ptr - (uintptr_t)_end) / 1024);
}
