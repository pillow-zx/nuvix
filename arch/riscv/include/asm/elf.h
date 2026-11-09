#ifndef _NUVIX_RISCV_ELF_H
#define _NUVIX_RISCV_ELF_H

#include <nuvix/elf.h>

#define EM_RISCV                  243

#define EF_RISCV_RVC              0x0001
#define EF_RISCV_FLOAT_ABI        0x0006
#define EF_RISCV_FLOAT_ABI_SOFT   0x0000
#define EF_RISCV_FLOAT_ABI_DOUBLE 0x0004

#define ELF_ARCH                  EM_RISCV
#define ELF_CLASS                 ELFCLASS64
#define ELF_DATA                  ELFDATA2LSB

#define ELF_HWCAP_FPU ((1UL << ('F' - 'A')) | (1UL << ('D' - 'A')))

#define ELF_HWCAP                                                              \
    ((1UL << ('I' - 'A')) | (1UL << ('M' - 'A')) | (1UL << ('A' - 'A')) |      \
     (1UL << ('C' - 'A')) | ELF_HWCAP_FPU)

static inline bool elf_check_arch(const struct elf64_ehdr *ehdr)
{
    unsigned int float_abi = ehdr->e_flags & EF_RISCV_FLOAT_ABI;

    if (float_abi > EF_RISCV_FLOAT_ABI_DOUBLE)
        return false;

    return ehdr->e_machine == ELF_ARCH &&
           ehdr->e_ident[EI_CLASS] == ELF_CLASS &&
           ehdr->e_ident[EI_DATA] == ELF_DATA &&
           !(ehdr->e_flags & ~(EF_RISCV_RVC | EF_RISCV_FLOAT_ABI));
}

#endif /* _NUVIX_RISCV_ELF_H */
