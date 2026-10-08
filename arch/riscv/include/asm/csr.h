#ifndef _NUVIX_RISCV_CSR_H
#define _NUVIX_RISCV_CSR_H

/* RV64 status fields; vector and optional privileged extensions are omitted. */
#define SSTATUS_SIE               (1 << 1)
#define SSTATUS_SPIE              (1 << 5)
#define SSTATUS_UBE               (1 << 6)
#define SSTATUS_SPP               (1 << 8)
#define SSTATUS_SPP_U             0
#define SSTATUS_SPP_S             SSTATUS_SPP

#define SSTATUS_FS_SHIFT          13
#define SSTATUS_FS                (3 << SSTATUS_FS_SHIFT)
#define SSTATUS_FS_OFF            (0 << SSTATUS_FS_SHIFT)
#define SSTATUS_FS_INITIAL        (1 << SSTATUS_FS_SHIFT)
#define SSTATUS_FS_CLEAN          (2 << SSTATUS_FS_SHIFT)
#define SSTATUS_FS_DIRTY          (3 << SSTATUS_FS_SHIFT)

#define SSTATUS_XS_SHIFT          15
#define SSTATUS_XS                (3 << SSTATUS_XS_SHIFT)
#define SSTATUS_XS_OFF            (0 << SSTATUS_XS_SHIFT)
#define SSTATUS_XS_INITIAL        (1 << SSTATUS_XS_SHIFT)
#define SSTATUS_XS_CLEAN          (2 << SSTATUS_XS_SHIFT)
#define SSTATUS_XS_DIRTY          (3 << SSTATUS_XS_SHIFT)

#define SSTATUS_SUM               (1 << 18)
#define SSTATUS_MXR               (1 << 19)
#define SSTATUS_UXL_SHIFT         32
#define SSTATUS_UXL               0x0000000300000000
#define SSTATUS_UXL_32            0x0000000100000000
#define SSTATUS_UXL_64            0x0000000200000000
#define SSTATUS_SD                0x8000000000000000

/* Supervisor software, timer and external interrupt enables/pending bits. */
#define SIE_SSIE                  (1 << 1)
#define SIE_STIE                  (1 << 5)
#define SIE_SEIE                  (1 << 9)
#define SIP_SSIP                  (1 << 1)
#define SIP_STIP                  (1 << 5)
#define SIP_SEIP                  (1 << 9)

/* The high bit selects interrupts; remaining bits hold the cause code. */
#define SCAUSE_INTERRUPT          0x8000000000000000
#define SCAUSE_CODE_MASK          0x7fffffffffffffff

/* Interrupt cause codes, without SCAUSE_INTERRUPT. */
#define SCAUSE_SSI                1
#define SCAUSE_STI                5
#define SCAUSE_SEI                9

/* Synchronous exception cause codes. */
#define SCAUSE_INST_MISALIGNED    0
#define SCAUSE_INST_ACCESS_FAULT  1
#define SCAUSE_ILLEGAL_INST       2
#define SCAUSE_BREAKPOINT         3
#define SCAUSE_LOAD_MISALIGNED    4
#define SCAUSE_LOAD_ACCESS_FAULT  5
#define SCAUSE_STORE_MISALIGNED   6
#define SCAUSE_STORE_ACCESS_FAULT 7
#define SCAUSE_ECALL_U            8
#define SCAUSE_ECALL_S            9
#define SCAUSE_INST_PAGE_FAULT    12
#define SCAUSE_LOAD_PAGE_FAULT    13
#define SCAUSE_STORE_PAGE_FAULT   15

#ifndef __ASSEMBLER__

/* CSR operands are assembler names or numbers, expanded before stringifying. */
#define __csr_name(csr) #csr

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
