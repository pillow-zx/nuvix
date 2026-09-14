#include <nuvix/printk.h>
#include <nuvix/buddy.h>
#include <nuvix/errno.h>
#include <nuvix/tools.h>
#include <nuvix/string.h>
#include <nuvix/math.h>
#include <nuvix/atomic.h>
#include <arch/page.h>
#include <arch/pgtable.h>
#include <asm/csr.h>

uintptr_t kpgroot;

atomic_isize_t pt_boot_token;

extern char _end[];

static inline bool pte_is_leaf(pte_t pte)
{
	return (pte & (PTE_R | PTE_W | PTE_X)) != 0;
}

static int pgtable_walk_create(pte_t *root, vaddr_t va, pte_t **out)
{
	int idx2 = (va >> 30) & 0x1FF;
	pte_t *l2e = &root[idx2];
	pte_t *l1;

	if (!(*l2e & PTE_V)) {
		l1 = get_free_page(0, ALLOC_NOWAIT);
		if (!l1)
			return -ENOMEM;
		memset(l1, 0, PAGE_SIZE);
		*l2e = PA_TO_PTE(__pa((uintptr_t)l1)) | PTE_TABLE;
	} else {
		if (pte_is_leaf(*l2e))
			return -EINVAL;
		l1 = (pte_t *)__va(PTE_TO_PA(*l2e));
	}

	int idx1 = (va >> 21) & 0x1FF;
	pte_t *l1e = &l1[idx1];
	pte_t *l0;

	if (!(*l1e & PTE_V)) {
		l0 = get_free_page(0, ALLOC_NOWAIT);
		/* An attached empty table stays owned by the root. Hardware may
		 * already be walking it; root retirement releases it safely. */
		if (!l0)
			return -ENOMEM;
		memset(l0, 0, PAGE_SIZE);
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

pte_t *pt_lookup(pte_t *root, vaddr_t va)
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

	ret = pgtable_walk_create(root, va, &pte);
	if (ret < 0)
		return ret;
	*pte = PA_TO_PTE(pa) | perm;
	return 0;
}

uintptr_t pt_boot_token_acquire(void)
{
	return (uintptr_t)atomic_isize_read_acquire(&pt_boot_token);
}

bool pt_boot_token_valid(void)
{
	uintptr_t token = pt_boot_token_acquire();

	return (token & SATP_MODE_SV39) == SATP_MODE_SV39 &&
	       (token & SATP_PPN_MASK) != 0;
}

pte_t *kpgtable(void)
{
	uintptr_t satp_val = kpgroot;
	uintptr_t root_pa = (satp_val & SATP_PPN_MASK) << PAGE_SHIFT;

	return (pte_t *)__va(root_pa);
}

void *pgtable_init(void)
{
	char *bootmem;
	pte_t *root = NULL;

	bootmem = (char *)ALIGN_UP((uintptr_t)_end, PAGE_SIZE);
	root = (pte_t *)bootmem;
	bootmem += PAGE_SIZE;
	memset(root, 0, PAGE_SIZE);

	pr_debug("page_table: mapping %dMB DRAM with 4KB pages...\n",
		(int)(DRAM_SIZE >> 20));

	for (paddr_t pa = DRAM_BASE; pa < DRAM_BASE + DRAM_SIZE;
	     pa += PAGE_SIZE) {
		vaddr_t va = KERNEL_VBASE + pa;
		int idx_high = (va >> 30) & 0x1FF;
		int idx_mid = (va >> 21) & 0x1FF;
		int idx_low = (va >> 12) & 0x1FF;
		pte_t *l1;
		pte_t *l0;

		if (!(root[idx_high] & PTE_V)) {
			l1 = (pte_t *)bootmem;
			bootmem += PAGE_SIZE;
			memset(l1, 0, PAGE_SIZE);
			root[idx_high] = PA_TO_PTE(__pa((uintptr_t)l1)) | PTE_TABLE;
		} else {
			l1 = (pte_t *)__va(PTE_TO_PA(root[idx_high]));
		}

		if (!(l1[idx_mid] & PTE_V)) {
			l0 = (pte_t *)bootmem;
			bootmem += PAGE_SIZE;
			memset(l0, 0, PAGE_SIZE);
			l1[idx_mid] = PA_TO_PTE(__pa((uintptr_t)l0)) | PTE_TABLE;
		} else {
			l0 = (pte_t *)__va(PTE_TO_PA(l1[idx_mid]));
		}

		l0[idx_low] = PA_TO_PTE(pa) | PTE_KERN_RWX;
	}

	int idx_high = ((KERNEL_VBASE + DRAM_BASE) >> 30) & 0x1FF;
	int idx_id = (DRAM_BASE >> 30) & 0x1FF;
	root[idx_id] = root[idx_high];

	root[0] = PA_TO_PTE(0UL) | PTE_KERN_RW;

	paddr_t root_pa = __pa((uintptr_t)root);
	uintptr_t satp_val = SATP_MODE_SV39 | (root_pa >> PAGE_SHIFT);
	kpgroot = satp_val;

	active_pgtable(satp_val);
	atomic_isize_set_release(&pt_boot_token, (isize)satp_val);

	pr_debug("page_table: switched to kernel page table (root=%p, "
		"bootmem=%p)\n",
		(void *)root_pa, bootmem);

	return bootmem;
}

__cold
void pgtable_udestroy(pte_t *pgd)
{
	for (int i = 0; i < 256; i++) {
		if (!pte_present(pgd[i]))
			continue;

		pte_t *pmd = (pte_t *)__va(PTE_TO_PA(pgd[i]));

		for (int j = 0; j < 512; j++) {
			if (!pte_present(pmd[j]))
				continue;

			pte_t *pt = (pte_t *)__va(PTE_TO_PA(pmd[j]));

			for (int k = 0; k < 512; k++)
				pt[k] = 0;
			free_page(pt, 0);
		}
		free_page(pmd, 0);
	}
	free_page(pgd, 0);
}

/* The caller owns the root; shared kernel entries are borrowed. */
pte_t *pgtable_ucreate(void)
{
	pte_t *root = get_free_page(0, ALLOC_NOWAIT);
	pte_t *kernel = kpgtable();

	if (!root)
		return NULL;
	memset(root, 0, PAGE_SIZE);
	for (int i = 256; i < 512; i++)
		root[i] = kernel[i];
	if (arch_upgd_init(root) < 0) {
		pgtable_udestroy(root);
		return NULL;
	}
	return root;
}

/* Boot-only preparation of leaf slots; no leaf mapping is published. */
int pgtable_prepare_range(pte_t *root, uintptr_t start, uintptr_t end)
{
	for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
		pte_t *pte;
		int ret = pgtable_walk_create(root, va, &pte);

		if (ret < 0)
			return ret;
	}
	return 0;
}

bool pgtable_take_upage(pte_t *root, uintptr_t *cursor, paddr_t *pa)
{
	uintptr_t va = *cursor;

	while (va < TASK_SIZE) {
		pte_t upper = root[(va >> 30) & 0x1ff];
		pte_t *middle, *leaves;
		pte_t entry;

		if (!pte_present(upper) || pte_is_leaf(upper)) {
			va = ALIGN_DOWN(va, 1UL << 30) + (1UL << 30);
			continue;
		}
		middle = __va(PTE_TO_PA(upper));
		entry = middle[(va >> 21) & 0x1ff];
		if (!pte_present(entry) || pte_is_leaf(entry)) {
			va = ALIGN_DOWN(va, 1UL << 21) + (1UL << 21);
			continue;
		}
		leaves = __va(PTE_TO_PA(entry));
		for (unsigned int i = (va >> PAGE_SHIFT) & 0x1ff;
		     i < 512 && va < TASK_SIZE; i++, va += PAGE_SIZE) {
			entry = leaves[i];
			if (!pte_user_page(entry))
				continue;
			leaves[i] = 0;
			*pa = PTE_TO_PA(entry);
			*cursor = va + PAGE_SIZE;
			return true;
		}
	}
	*cursor = va;
	return false;
}
