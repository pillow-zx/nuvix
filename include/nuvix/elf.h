#ifndef _NUVIX_ELF_H
#define _NUVIX_ELF_H

/**
 * @file elf.h
 * @brief ELF64 constants and structures used by exec loading.
 */

#include <nuvix/types.h>

#define ELFMAG0		0x7f
#define ELFMAG1		'E'
#define ELFMAG2		'L'
#define ELFMAG3		'F'
#define ELFMAG		"\x7f""ELF"
#define SELFMAG		4

#define EI_MAG0	      0
#define EI_MAG1	      1
#define EI_MAG2	      2
#define EI_MAG3	      3
#define EI_CLASS      4
#define EI_DATA	      5
#define EI_VERSION    6
#define EI_OSABI      7
#define EI_ABIVERSION 8
#define EI_PAD	      9
#define EI_NIDENT     16

#define ELFCLASSNONE 0
#define ELFCLASS32   1
#define ELFCLASS64   2

#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

#define ET_NONE		0
#define ET_REL		1
#define ET_EXEC		2
#define ET_DYN		3
#define ET_CORE		4

#define EM_NONE		0
#define EM_RISCV	243

#define EV_NONE		0
#define EV_CURRENT	1

/**
 * @typedef Elf64_Ehdr
 * @brief ELF64 file header layout.
 *
 * @par Fields
 * - @c e_ident: Magic, class, endian, version.
 * - @c e_type: Object file type, such as ET_EXEC.
 * - @c e_machine: Target machine, EM_RISCV for nuvix userspace.
 * - @c e_version: ELF version, expected EV_CURRENT.
 * - @c e_entry: User entry virtual address.
 * - @c e_phoff: Program-header table file offset.
 * - @c e_shoff: Section-header table file offset.
 * - @c e_flags: Architecture-specific ELF flags.
 * - @c e_ehsize: ELF header size in bytes.
 * - @c e_phentsize: One program-header entry size.
 * - @c e_phnum: Number of program-header entries.
 * - @c e_shentsize: One section-header entry size.
 * - @c e_shnum: Number of section-header entries.
 * - @c e_shstrndx: Section-name string table index.
 */
typedef struct {
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
} Elf64_Ehdr;

#define PT_NULL		0
#define PT_LOAD		1
#define PT_DYNAMIC	2
#define PT_INTERP	3
#define PT_NOTE		4
#define PT_SHLIB	5
#define PT_PHDR		6
#define PT_TLS		7

#define PF_X		0x1
#define PF_W		0x2
#define PF_R		0x4

/**
 * @typedef Elf64_Phdr
 * @brief ELF64 program header layout.
 *
 * @par Fields
 * - @c p_type: Segment type, such as PT_LOAD.
 * - @c p_flags: PF_R/PF_W/PF_X segment permissions.
 * - @c p_offset: Segment bytes file offset.
 * - @c p_vaddr: User virtual load address.
 * - @c p_paddr: Physical address field, ignored for userspace.
 * - @c p_filesz: Bytes present in the file image.
 * - @c p_memsz: Bytes required in memory, including BSS.
 * - @c p_align: Required file/virtual alignment.
 */
typedef struct {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
} Elf64_Phdr;

#endif
