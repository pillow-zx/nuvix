#ifndef _NUVIX_RISCV_ELF_H
#define _NUVIX_RISCV_ELF_H

#include <nuvix/elf.h>

#define EM_RISCV                  243

#define EF_RISCV_RVC              0x0001
#define EF_RISCV_FLOAT_ABI        0x0006
#define EF_RISCV_FLOAT_ABI_DOUBLE 0x0004

#define ELF_ARCH                  EM_RISCV
#define ELF_CLASS                 ELFCLASS64
#define ELF_DATA                  ELFDATA2LSB

static inline bool elf_check_arch(const struct elf64_ehdr *ehdr)
{
    return ehdr->e_machine == ELF_ARCH &&
           ehdr->e_ident[EI_CLASS] == ELF_CLASS &&
           ehdr->e_ident[EI_DATA] == ELF_DATA &&
           (ehdr->e_flags & EF_RISCV_FLOAT_ABI) == EF_RISCV_FLOAT_ABI_DOUBLE &&
           !(ehdr->e_flags & ~(EF_RISCV_RVC | EF_RISCV_FLOAT_ABI));
}

#endif /* _NUVIX_RISCV_ELF_H */
