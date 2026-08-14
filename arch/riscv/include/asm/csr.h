#ifndef _NUVIX_ASM_CSR_H
#define _NUVIX_ASM_CSR_H

/**
 * @file
 * @brief RISC-V CSR bit definitions and inline access helpers.
 */

#include <nuvix/types.h>
#include <nuvix/bitops.h>

/** @def SSTATUS_SPP
 * @brief Previous privilege mode bit in sstatus; set means return to S-mode.
 */
#define SSTATUS_SPP  BIT(8)
/** @def SSTATUS_SPIE
 * @brief Previous supervisor interrupt-enable bit restored by sret.
 */
#define SSTATUS_SPIE BIT(5)
/** @def SSTATUS_SIE
 * @brief Current supervisor interrupt-enable bit.
 */
#define SSTATUS_SIE  BIT(1)
/** @def SSTATUS_FS_MASK
 * @brief Floating-point state field. Clear it to make F/D instructions trap.
 */
#define SSTATUS_FS_SHIFT 13
#define SSTATUS_FS_MASK  (3UL << SSTATUS_FS_SHIFT)
/** @def SSTATUS_SUM
 * @brief Permit S-mode explicit memory accesses to U-mode pages.
 */
#define SSTATUS_SUM  BIT(18)

/** @def SIE_SSIE
 * @brief Supervisor software interrupt-enable bit in sie.
 */
#define SIE_SSIE BIT(1)
/** @def SIP_SSIP
 * @brief Supervisor software interrupt pending bit in sip (write 1 to clear).
 */
#define SIP_SSIP BIT(1)
/** @def SIE_STIE
 * @brief Supervisor timer interrupt-enable bit in sie.
 */
#define SIE_STIE BIT(5)
/** @def SIE_SEIE
 * @brief Supervisor external interrupt-enable bit in sie.
 */
#define SIE_SEIE BIT(9)

/** @def SATP_MODE_SV39
 * @brief satp MODE field value selecting Sv39 on RV64.
 */
#define SATP_MODE_SV39 (8UL << 60)
/** @def SATP_PPN_BITS
 * @brief Number of physical page number bits available in satp on RV64.
 */
#define SATP_PPN_BITS  44
/** @def SATP_PPN_MASK
 * @brief Mask for the satp physical page number field.
 */
#define SATP_PPN_MASK  MASK(SATP_PPN_BITS)

/**
 * @def csr_read
 * @brief Read a CSR with a compiler memory clobber.
 * @param csr CSR symbolic name accepted by the assembler.
 * @return The CSR value as size_t.
 */
#define csr_read(csr)                                                          \
	({                                                                     \
		size_t __v;                                                    \
		asm volatile("csrr %0, " #csr : "=r"(__v) : : "memory");       \
		__v;                                                           \
	})

/**
 * @def csr_write
 * @brief Write a CSR with a compiler memory clobber.
 * @param csr CSR symbolic name accepted by the assembler.
 * @param val Value written to the CSR.
 */
#define csr_write(csr, val)                                                    \
	({ asm volatile("csrw " #csr ", %0" ::"rK"(val) : "memory"); })

/**
 * @def csr_set
 * @brief Atomically set selected bits in a CSR.
 * @param csr CSR symbolic name accepted by the assembler.
 * @param bits Bit mask to set.
 */
#define csr_set(csr, bits)                                                     \
	({ asm volatile("csrs " #csr ", %0" ::"rK"(bits) : "memory"); })

/**
 * @def csr_clear
 * @brief Atomically clear selected bits in a CSR.
 * @param csr CSR symbolic name accepted by the assembler.
 * @param bits Bit mask to clear.
 */
#define csr_clear(csr, bits)                                                   \
	({ asm volatile("csrc " #csr ", %0" ::"rK"(bits) : "memory"); })

/**
 * @def barrier
 * @brief Compiler-only memory barrier.
 */
#define barrier() ({ asm volatile("" ::: "memory"); })

/**
 * @def wfi
 * @brief Execute the RISC-V wait-for-interrupt instruction.
 */
#define wfi() ({ asm volatile("wfi"); })

void tlb_flush_all(void);
void tlb_flush_page(uintptr_t va);
void icache_flush(void);

#endif
