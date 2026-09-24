#ifndef _NUVIX_PGTABLE_H
#define _NUVIX_PGTABLE_H

#include <arch/pgtable.h>

void pgtable_init(void);

pte_t *pgtable_create(void);

void pgtable_destroy(pte_t *root);

bool pgtable_take_upage(pte_t *root, uintptr_t *cursor, paddr_t *pa);

int pgtable_prepare_range(pte_t *root, uintptr_t start, uintptr_t end);

__must_check
pte_t *kpgtable(void);

__must_check __nonnull(1)
pte_t *pt_lookup(pte_t *root, uintptr_t va);

__must_check __nonnull(1)
int map_page(pte_t *root, uintptr_t va, uintptr_t pa, pgprot_t perm);

#endif
