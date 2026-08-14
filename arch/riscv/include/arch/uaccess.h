#ifndef _NUVIX_ARCH_RISCV_UACCESS_H
#define _NUVIX_ARCH_RISCV_UACCESS_H

#include <asm/csr.h>
#include <nuvix/compiler.h>
#include <nuvix/types.h>

__always_inline __must_check
static inline bool user_access_begin(void)
{
	bool had_sum = (csr_read(sstatus) & SSTATUS_SUM) != 0;

	if (!had_sum)
		csr_set(sstatus, SSTATUS_SUM);
	return had_sum;
}

__always_inline
static inline void user_access_end(bool had_sum)
{
	if (!had_sum)
		csr_clear(sstatus, SSTATUS_SUM);
}

#endif
