#include <nuvix/elf.h>
#include <nuvix/errno.h>
#include <nuvix/string.h>
#include <nuvix/assert.h>
#include <asm/elf.h>

static_assert(sizeof(struct elf64_ehdr) == 64, "invalid ELF64 header layout");
static_assert(sizeof(struct elf64_phdr) == 56,
              "invalid ELF64 program header layout");

static bool file_range_valid(size_t size, uint64_t offset, uint64_t length)
{
    return offset <= size && length <= size - offset;
}

int elf_get_phdr(const struct elf_image *image, unsigned int index,
                 struct elf64_phdr *phdr)
{
    if (!image || !phdr || index >= image->ehdr.e_phnum)
        return -EINVAL;
    memcpy(phdr,
           (const unsigned char *)image->data + image->ehdr.e_phoff +
               (uint64_t)index * sizeof(*phdr),
           sizeof(*phdr));
    return 0;
}

static bool segment_valid(const struct elf_image *image,
                          const struct elf64_phdr *phdr)
{
    if (phdr->p_filesz > phdr->p_memsz ||
        !file_range_valid(image->size, phdr->p_offset, phdr->p_filesz) ||
        phdr->p_memsz > UINT64_MAX - phdr->p_vaddr ||
        (phdr->p_flags & ~(PF_R | PF_W | PF_X)))
        return false;
    if (phdr->p_align > 1) {
        uint64_t mask = phdr->p_align - 1;

        if ((phdr->p_align & mask) ||
            (phdr->p_vaddr & mask) != (phdr->p_offset & mask))
            return false;
    }
    return true;
}

int elf_parse(const void *data, size_t size, struct elf_image *image)
{
    struct elf_image parsed = {.data = data, .size = size};
    struct elf64_ehdr *ehdr = &parsed.ehdr;
    uint64_t phsize, load_end = 0;
    bool have_entry = false, have_load = false;

    if (!data || !image)
        return -EINVAL;
    if (size < sizeof(*ehdr))
        return -ENOEXEC;
    memcpy(ehdr, data, sizeof(*ehdr));
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) || !elf_check_arch(ehdr) ||
        ehdr->e_ident[EI_VERSION] != EV_CURRENT ||
        ehdr->e_version != EV_CURRENT ||
        (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) ||
        ehdr->e_ehsize != sizeof(*ehdr) ||
        ehdr->e_phentsize != sizeof(struct elf64_phdr) || !ehdr->e_phnum ||
        ehdr->e_phnum == PN_XNUM || ehdr->e_phoff < sizeof(*ehdr))
        return -ENOEXEC;
    phsize = (uint64_t)ehdr->e_phnum * sizeof(struct elf64_phdr);
    if (!file_range_valid(size, ehdr->e_phoff, phsize))
        return -ENOEXEC;

    for (unsigned int i = 0; i < ehdr->e_phnum; ++i) {
        struct elf64_phdr phdr;

        elf_get_phdr(&parsed, i, &phdr);
        if ((phdr.p_type == PT_DYNAMIC || phdr.p_type == PT_LOAD ||
             phdr.p_type == PT_PHDR || phdr.p_type == PT_TLS ||
             phdr.p_type == PT_GNU_RELRO) &&
            !segment_valid(&parsed, &phdr))
            return -ENOEXEC;

        if (phdr.p_type == PT_INTERP || phdr.p_type == PT_SHLIB) {
            return -ENOEXEC;
        } else if (phdr.p_type == PT_LOAD) {
            if (have_load && phdr.p_vaddr < load_end)
                return -ENOEXEC;
            have_load = true;
            load_end = phdr.p_vaddr + phdr.p_memsz;
            if ((phdr.p_flags & PF_X) && ehdr->e_entry >= phdr.p_vaddr &&
                ehdr->e_entry - phdr.p_vaddr < phdr.p_memsz)
                have_entry = true;
        } else if (phdr.p_type == PT_PHDR) {
            if (have_load || phdr.p_offset != ehdr->e_phoff ||
                phdr.p_filesz != phsize ||
                phdr.p_memsz != phsize)
                return -ENOEXEC;
        } else if (phdr.p_type == PT_GNU_STACK) {
            if ((phdr.p_flags & PF_X) ||
                (phdr.p_flags & ~(PF_R | PF_W | PF_X)))
                return -ENOEXEC;
        }
    }
    if (!have_entry)
        return -ENOEXEC;
    *image = parsed;
    return 0;
}
