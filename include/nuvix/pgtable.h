#ifndef _NUVIX_PGTABLE_H
#define _NUVIX_PGTABLE_H

#include <nuvix/arch/riscv/pgtable.h>

struct dtb;

/*
 * Called once after page_init(), with valid DTB and boot-accessible RAM.
 * Failed builds leave allocations reserved.
 */
int pgtable_init(struct dtb *dtb);

#endif /* ifndef _NUVIX_PGTABLE_H */
