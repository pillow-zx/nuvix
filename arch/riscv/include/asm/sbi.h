#ifndef _NUVIX_RISCV_SBI_H
#define _NUVIX_RISCV_SBI_H

#include <nuvix/compiler.h>

#define SBI_MAX_ARGS                     6

#define SBI_EXT_0_1_CONSOLE_PUTCHAR      0x01
#define SBI_EXT_0_1_CONSOLE_GETCHAR      0x02

#define SBI_EXT_BASE                     0x10
#define SBI_EXT_BASE_PROBE_EXT           3
#define SBI_EXT_IPI                      0x735049
#define SBI_EXT_IPI_SEND_IPI             0
#define SBI_EXT_TIME                     0x54494D45
#define SBI_EXT_TIME_SET_TIMER           0
#define SBI_EXT_HSM                      0x48534D
#define SBI_EXT_HSM_HART_START           0
#define SBI_EXT_RFENCE                   0x52464E43
#define SBI_EXT_RFENCE_REMOTE_FENCE_I    0
#define SBI_EXT_RFENCE_REMOTE_SFENCE_VMA 1

struct sbiret {
    long error;
    long value;
};

static __always_inline struct sbiret
__sbi_call(int eid, int fid, const unsigned long args[SBI_MAX_ARGS])
{
    register unsigned long a0 __asm__("a0") = args[0];
    register unsigned long a1 __asm__("a1") = args[1];
    register unsigned long a2 __asm__("a2") = args[2];
    register unsigned long a3 __asm__("a3") = args[3];
    register unsigned long a4 __asm__("a4") = args[4];
    register unsigned long a5 __asm__("a5") = args[5];
    register unsigned long a6 __asm__("a6") = (unsigned long)fid;
    register unsigned long a7 __asm__("a7") = (unsigned long)eid;

    __asm__ volatile("ecall"
                     : "+r"(a0), "+r"(a1)
                     : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
                     : "memory");

    return (struct sbiret){.error = (long)a0, .value = (long)a1};
}

#define sbi_call(eid, fid, ...)                                                \
    __sbi_call((eid), (fid), (const unsigned long[SBI_MAX_ARGS]){__VA_ARGS__})

static inline struct sbiret sbi_probe_extension(int eid)
{
    return sbi_call(SBI_EXT_BASE, SBI_EXT_BASE_PROBE_EXT, (unsigned long)eid);
}

static inline struct sbiret sbi_send_ipi(unsigned long hart_mask,
                                         unsigned long hart_mask_base)
{
    return sbi_call(SBI_EXT_IPI, SBI_EXT_IPI_SEND_IPI, hart_mask,
                    hart_mask_base);
}

static inline struct sbiret sbi_set_timer(unsigned long deadline)
{
    return sbi_call(SBI_EXT_TIME, SBI_EXT_TIME_SET_TIMER, deadline);
}

static inline struct sbiret
sbi_hart_start(unsigned long hart_id, unsigned long entry, unsigned long opaque)
{
    return sbi_call(SBI_EXT_HSM, SBI_EXT_HSM_HART_START, hart_id, entry,
                    opaque);
}

static inline struct sbiret sbi_remote_sfence_vma(unsigned long hart_mask,
                                                  unsigned long hart_mask_base,
                                                  unsigned long start_addr,
                                                  unsigned long size)
{
    return sbi_call(SBI_EXT_RFENCE, SBI_EXT_RFENCE_REMOTE_SFENCE_VMA, hart_mask,
                    hart_mask_base, start_addr, size);
}

static inline struct sbiret sbi_remote_fence_i(unsigned long hart_mask,
                                               unsigned long hart_mask_base)
{
    return sbi_call(SBI_EXT_RFENCE, SBI_EXT_RFENCE_REMOTE_FENCE_I, hart_mask,
                    hart_mask_base);
}

static inline int sbi_putchar(int c)
{
    long ret = sbi_call(SBI_EXT_0_1_CONSOLE_PUTCHAR, 0, (unsigned char)c).error;

    return ret < 0 ? (int)ret : (unsigned char)c;
}

static inline int sbi_getchar(void)
{
    return (int)sbi_call(SBI_EXT_0_1_CONSOLE_GETCHAR, 0).error;
}

#endif /* _NUVIX_RISCV_SBI_H */
