#ifndef _NUVIX_RISCV_CSR_H
#define _NUVIX_RISCV_CSR_H

#define SSTATUS_SIE (1 << 1)
#define SSTATUS_FS  (3 << 13)
#define SIE_SSIE    (1 << 1)
#define SIE_STIE    (1 << 5)
#define SIP_SSIP    (1 << 1)
#define SCAUSE_SSI  1
#define SCAUSE_STI  5

#ifndef __ASSEMBLER__

#define SCAUSE_INTERRUPT (1UL << 63)

/* CSR operands are assembler names or numbers, expanded before stringifying. */
#define __csr_name(csr)  #csr

/* These compiler barriers do not replace hardware memory/TLB fences. */
#define csr_read(csr)                                                          \
    ({                                                                         \
        unsigned long __csr_value;                                             \
                                                                               \
        __asm__ volatile("csrr %0, " __csr_name(csr)                           \
                         : "=r"(__csr_value)                                   \
                         :                                                     \
                         : "memory");                                          \
        __csr_value;                                                           \
    })

#define csr_write(csr, value)                                                  \
    do {                                                                       \
        unsigned long __csr_value = (unsigned long)(value);                    \
                                                                               \
        __asm__ volatile("csrw " __csr_name(csr) ", %0"                        \
                         :                                                     \
                         : "rK"(__csr_value)                                   \
                         : "memory");                                          \
    } while (0)

#define csr_set(csr, bits)                                                     \
    do {                                                                       \
        unsigned long __csr_bits = (unsigned long)(bits);                      \
                                                                               \
        __asm__ volatile("csrs " __csr_name(csr) ", %0"                        \
                         :                                                     \
                         : "rK"(__csr_bits)                                    \
                         : "memory");                                          \
    } while (0)

#define csr_clear(csr, bits)                                                   \
    do {                                                                       \
        unsigned long __csr_bits = (unsigned long)(bits);                      \
                                                                               \
        __asm__ volatile("csrc " __csr_name(csr) ", %0"                        \
                         :                                                     \
                         : "rK"(__csr_bits)                                    \
                         : "memory");                                          \
    } while (0)

/* Read the old value and clear bits in a single CSR instruction. */
#define csr_read_clear(csr, bits)                                              \
    ({                                                                         \
        unsigned long __csr_bits = (unsigned long)(bits);                      \
        unsigned long __csr_value;                                             \
                                                                               \
        __asm__ volatile("csrrc %0, " __csr_name(csr) ", %1"                   \
                         : "=r"(__csr_value)                                   \
                         : "rK"(__csr_bits)                                    \
                         : "memory");                                          \
        __csr_value;                                                           \
    })

#endif /* __ASSEMBLER__ */

#endif /* _NUVIX_RISCV_CSR_H */
