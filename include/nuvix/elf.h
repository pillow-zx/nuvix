#ifndef _NUVIX_ELF_H
#define _NUVIX_ELF_H

#include <nuvix/ctypes.h>

#define EI_NIDENT       16
#define EI_CLASS        4
#define EI_DATA         5
#define EI_VERSION      6

#define ELFMAG          "\177ELF"
#define SELFMAG         4
#define ELFCLASS64      2
#define ELFDATA2LSB     1
#define EV_CURRENT      1
#define ET_EXEC         2
#define ET_DYN          3
#define PN_XNUM         0xffff

#define PT_NULL         0
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_SHLIB        5
#define PT_PHDR         6
#define PT_TLS          7
#define PT_GNU_STACK    0x6474e551
#define PT_GNU_RELRO    0x6474e552

#define PF_X            0x1
#define PF_W            0x2
#define PF_R            0x4

#define AT_NULL         0
#define AT_PHDR         3
#define AT_PHENT        4
#define AT_PHNUM        5
#define AT_PAGESZ       6
#define AT_BASE         7
#define AT_ENTRY        9
#define AT_UID          11
#define AT_EUID         12
#define AT_GID          13
#define AT_EGID         14
#define AT_HWCAP        16
#define AT_SECURE       23
#define AT_RANDOM       25
#define AT_EXECFN       31

#define ELF_RANDOM_SIZE 16

struct elf64_ehdr {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

struct elf_image {
    const void *data;
    size_t size;
    struct elf64_ehdr ehdr;
};

int elf_parse(const void *data, size_t size, struct elf_image *image);

int elf_check_ehdr(const struct elf64_ehdr *ehdr, size_t file_limit);
int elf_check_phdrs(const struct elf64_ehdr *ehdr, const void *phdrs,
                    size_t file_limit);

int elf_get_phdr(const struct elf_image *image, unsigned int index,
                 struct elf64_phdr *phdr);

#endif /* _NUVIX_ELF_H */
