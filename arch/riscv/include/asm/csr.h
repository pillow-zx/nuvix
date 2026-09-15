#ifndef _NUVIX_ASM_CSR_H
#define _NUVIX_ASM_CSR_H

#include <nuvix/types.h>
#include <nuvix/bitops.h>

#define SSTATUS_SPP	 BIT(8)
#define SSTATUS_SPIE	 BIT(5)
#define SSTATUS_SIE	 BIT(1)
#define SSTATUS_FS_SHIFT 13
#define SSTATUS_FS_MASK	 (3UL << SSTATUS_FS_SHIFT)
#define SSTATUS_SUM	 BIT(18)

#define SIE_SSIE        BIT(1)
#define SIP_SSIP        BIT(1)
#define SIE_STIE        BIT(5)
#define SIE_SEIE        BIT(9)

#define SATP_MODE_SV39  (8UL << 60)
#define SATP_PPN_BITS   44
#define SATP_PPN_MASK   MASK(SATP_PPN_BITS)

#define csr_read(csr)                                                          \
	({                                                                     \
		size_t __v;                                                    \
		asm volatile("csrr %0, " #csr : "=r"(__v) : : "memory");       \
		__v;                                                           \
	})

#define csr_write(csr, val)                                                    \
	({ asm volatile("csrw " #csr ", %0" ::"rK"(val) : "memory"); })

#define csr_set(csr, bits)                                                     \
	({ asm volatile("csrs " #csr ", %0" ::"rK"(bits) : "memory"); })

#define csr_clear(csr, bits)                                                   \
	({ asm volatile("csrc " #csr ", %0" ::"rK"(bits) : "memory"); })

#define barrier() ({ asm volatile("" ::: "memory"); })

#define wfi() ({ asm volatile("wfi"); })

#endif
