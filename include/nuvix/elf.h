#ifndef _NUVIX_ELF_H
#define _NUVIX_ELF_H

#include <nuvix/ctypes.h>

#define EI_NIDENT    16
#define EI_CLASS     4
#define EI_DATA      5
#define EI_VERSION   6

#define ELFMAG       "\177ELF"
#define SELFMAG      4
#define ELFCLASS64   2
#define ELFDATA2LSB  1
#define EV_CURRENT   1
#define ET_EXEC      2
#define ET_DYN       3
#define PN_XNUM      0xffff

#define PT_NULL      0
#define PT_LOAD      1
#define PT_DYNAMIC   2
#define PT_INTERP    3
#define PT_SHLIB     5
#define PT_PHDR      6
#define PT_TLS       7
#define PT_GNU_STACK 0x6474e551
#define PT_GNU_RELRO 0x6474e552

#define PF_X         0x1
#define PF_W         0x2
#define PF_R         0x4

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
    /* Borrowed kernel buffer; keep it alive and unchanged while using this view. */
    const void *data;
    size_t size;
    struct elf64_ehdr ehdr;
};

/*
 * Parses the current architecture's ET_EXEC or ET_DYN image without PT_INTERP,
 * without allocation. ET_DYN may carry PT_DYNAMIC for user-space self-relocation;
 * dynamic entries, dependencies and relocations are not interpreted.
 * The buffer need not be aligned. Returns -EINVAL for NULL arguments and
 * -ENOEXEC for malformed or unsupported input; leaves *image unchanged on error.
 * Checks byte ranges, load ordering/overlap and an executable entry. The loader
 * must enforce user layout, page-level mapping rules and auxiliary segment
 * coverage. Section headers and optional metadata contents are not interpreted.
 */
int elf_parse(const void *data, size_t size, struct elf_image *image);

/*
 * Requires an unmodified view returned by elf_parse(). Copies one program header
 * by table index, including load, dynamic, PHDR, TLS and RELRO information.
 * Returns -EINVAL for NULL arguments or an invalid index, leaving *phdr unchanged.
 */
int elf_get_phdr(const struct elf_image *image, unsigned int index,
                 struct elf64_phdr *phdr);

#endif /* _NUVIX_ELF_H */
