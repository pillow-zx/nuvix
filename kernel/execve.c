#include <nuvix/execve.h>
#include <nuvix/errno.h>
#include <nuvix/slab.h>
#include <nuvix/math.h>
#include <nuvix/elf.h>
#include <nuvix/task.h>
#include <nuvix/string.h>
#include <nuvix/pgtable.h>
#include <nuvix/mm.h>
#include <nuvix/page.h>
#include <nuvix/vfs.h>
#include <nuvix/sched.h>
#include <asm/elf.h>

#define ELF_MAGIC_NUMBER 0x9e3779b97f4a7c15ULL

static const uint64_t initial_random[ELF_RANDOM_SIZE / sizeof(uint64_t)] = {
    ELF_MAGIC_NUMBER, ELF_MAGIC_NUMBER};

struct exec_image {
    struct elf64_ehdr ehdr;
    struct elf64_phdr *phdrs;
    vaddr_t phdr_addr;
    vaddr_t relro_start;
    vaddr_t relro_end;
};

static int read_exact(struct file *file, void *buffer, size_t size,
                      uint64_t offset)
{
    loff_t pos = (loff_t)offset;
    char *data = buffer;

    while (size) {
        ssize_t ret = file->f_ops->read(file, data, size, &pos);

        if (ret < 0)
            return (int)ret;
        if (!ret)
            return -ENOEXEC;
        data += ret;
        size -= ret;
    }
    return 0;
}

static const struct elf64_phdr *find_load(const struct exec_image *image,
                                          vaddr_t addr, size_t size)
{
    for (unsigned int i = 0; i < image->ehdr.e_phnum; ++i) {
        const struct elf64_phdr *phdr = &image->phdrs[i];

        if (phdr->p_type == PT_LOAD && addr >= phdr->p_vaddr &&
            addr - phdr->p_vaddr <= phdr->p_memsz &&
            size <= phdr->p_memsz - (addr - phdr->p_vaddr))
            return phdr;
    }
    return NULL;
}

static int check_layout(struct exec_image *image)
{
    size_t phsize = (size_t)image->ehdr.e_phnum * sizeof(*image->phdrs);
    vaddr_t load_end = UVADDR_START;
    bool have_relro = false;

    for (unsigned int i = 0; i < image->ehdr.e_phnum; ++i) {
        const struct elf64_phdr *phdr = &image->phdrs[i];
        vaddr_t start, end;

        if (phdr->p_type == PT_DYNAMIC)
            return -ENOEXEC;
        if (phdr->p_type != PT_LOAD || !phdr->p_filesz)
            continue;
        if (phdr->p_vaddr < UVADDR_START ||
            phdr->p_vaddr + phdr->p_memsz > USTACK_BOTTOM - PAGE_SIZE ||
            ((phdr->p_vaddr ^ phdr->p_offset) & (PAGE_SIZE - 1)))
            return -ENOEXEC;
        start = ALIGN_DOWN(phdr->p_vaddr, (vaddr_t)PAGE_SIZE);
        end = ALIGN_UP(phdr->p_vaddr + phdr->p_memsz, (vaddr_t)PAGE_SIZE);
        if (start < load_end)
            return -ENOEXEC;
        load_end = end;
        if ((phdr->p_flags & PF_R) && image->ehdr.e_phoff >= phdr->p_offset &&
            image->ehdr.e_phoff - phdr->p_offset <= phdr->p_filesz &&
            phsize <= phdr->p_filesz - (image->ehdr.e_phoff - phdr->p_offset))
            image->phdr_addr =
                phdr->p_vaddr + (image->ehdr.e_phoff - phdr->p_offset);
    }

    for (unsigned int i = 0; i < image->ehdr.e_phnum; ++i) {
        const struct elf64_phdr *phdr = &image->phdrs[i];
        const struct elf64_phdr *load;

        if (phdr->p_type == PT_PHDR) {
            if (!image->phdr_addr || phdr->p_vaddr != image->phdr_addr)
                return -ENOEXEC;
        } else if (phdr->p_type == PT_TLS && phdr->p_filesz) {
            load = find_load(image, phdr->p_vaddr, phdr->p_filesz);
            if (!load || !(load->p_flags & PF_R) ||
                phdr->p_vaddr - load->p_vaddr > load->p_filesz ||
                phdr->p_filesz >
                    load->p_filesz - (phdr->p_vaddr - load->p_vaddr) ||
                phdr->p_offset !=
                    load->p_offset + (phdr->p_vaddr - load->p_vaddr))
                return -ENOEXEC;
        } else if (phdr->p_type == PT_GNU_RELRO && phdr->p_memsz) {
            load = find_load(image, phdr->p_vaddr, phdr->p_memsz);
            if (have_relro || !load || !(load->p_flags & PF_R))
                return -ENOEXEC;
            have_relro = true;
            image->relro_start = ALIGN_DOWN(phdr->p_vaddr, (vaddr_t)PAGE_SIZE);
            image->relro_end =
                ALIGN_DOWN(phdr->p_vaddr + phdr->p_memsz, (vaddr_t)PAGE_SIZE);
        }
    }
    return 0;
}

static int load_range(struct mm_struct *mm, struct file *file,
                      const struct elf64_phdr *phdr, vaddr_t start, vaddr_t end,
                      unsigned long flags)
{
    struct vm_area_struct *vma __free(vma) = vma_alloc();
    vaddr_t data_start = MAX(start, phdr->p_vaddr);
    vaddr_t data_end = MIN(end, phdr->p_vaddr + phdr->p_filesz);
    int ret;

    if (!vma)
        return -ENOMEM;
    vma->vm_start = start;
    vma->vm_end = end;
    vma->vm_flags |= flags;
    if (data_start < data_end) {
        size_t nr_pages = DIV_ROUND_UP(data_end - start, PAGE_SIZE);

        vma->vm_pages = kzalloc(nr_pages * sizeof(*vma->vm_pages));
        if (!vma->vm_pages)
            return -ENOMEM;
        vma->vm_nr_pages = nr_pages;

        for (size_t i = 0; i < nr_pages; ++i) {
            struct page *page = alloc_page();
            vaddr_t addr = start + i * PAGE_SIZE;
            vaddr_t first = MAX(addr, data_start);
            vaddr_t last = MIN(addr + PAGE_SIZE, data_end);
            void *data;

            if (!page)
                return -ENOMEM;
            vma->vm_pages[i] = page;
            data = (void *)__va(page_to_phys(page));
            memset(data, 0, PAGE_SIZE);
            ret = read_exact(file, (char *)data + (first - addr), last - first,
                             phdr->p_offset + first - phdr->p_vaddr);
            if (ret)
                return ret;
        }
    }
    ret = vma_insert(mm, vma);
    if (!ret)
        keep(vma);
    return ret;
}

static int load_segments(struct mm_struct *mm, struct file *file,
                         const struct exec_image *image)
{
    for (unsigned int i = 0; i < image->ehdr.e_phnum; ++i) {
        const struct elf64_phdr *phdr = &image->phdrs[i];
        unsigned long flags = 0;
        vaddr_t start, end;

        if (phdr->p_type != PT_LOAD || !phdr->p_memsz)
            continue;
        if (phdr->p_flags & PF_R)
            flags |= VM_READ;
        if (phdr->p_flags & PF_W)
            flags |= VM_READ | VM_WRITE;
        if (phdr->p_flags & PF_X)
            flags |= VM_EXEC;
        start = ALIGN_DOWN(phdr->p_vaddr, (vaddr_t)PAGE_SIZE);
        end = ALIGN_UP(phdr->p_vaddr + phdr->p_memsz, (vaddr_t)PAGE_SIZE);

        while (start < end) {
            vaddr_t stop = end;
            unsigned long prot = flags;
            int ret;

            if (start < image->relro_start)
                stop = MIN(stop, image->relro_start);
            else if (start < image->relro_end) {
                stop = MIN(stop, image->relro_end);
                prot &= ~VM_WRITE;
            }
            ret = load_range(mm, file, phdr, start, stop, prot);
            if (ret)
                return ret;
            start = stop;
        }
    }
    return 0;
}

static int stack_copy(struct mm_struct *mm, vaddr_t addr, const void *src,
                      size_t size)
{
    const char *data = src;

    while (size) {
        struct page *page __free(page) = NULL;
        size_t count = MIN(size, PAGE_SIZE - (addr & (PAGE_SIZE - 1)));
        paddr_t paddr;
        int ret = pgtable_lookup(mm, addr, &paddr);

        if (ret) {
            Assert(ret == -ENOENT, "invalid initial stack address\n");
            page = alloc_page();
            if (!page)
                return -ENOMEM;
            paddr = page_to_phys(page);
            memset((void *)__va(paddr), 0, PAGE_SIZE);
            ret = pgtable_map_page(mm, ALIGN_DOWN(addr, (vaddr_t)PAGE_SIZE),
                                   page, VM_READ | VM_WRITE | VM_ANON);
            if (ret)
                return ret;
            keep(page);
            paddr += addr & (PAGE_SIZE - 1);
        }
        memcpy((void *)__va(paddr), data, count);
        addr += count;
        data += count;
        size -= count;
    }
    return 0;
}

static int stack_push(struct mm_struct *mm, vaddr_t *sp, const void *src,
                      size_t size)
{
    if (size > *sp - USTACK_BOTTOM)
        return -E2BIG;
    *sp -= size;
    return stack_copy(mm, *sp, src, size);
}

static int stack_string(struct mm_struct *mm, vaddr_t *sp, const char *str)
{
    size_t available = *sp - USTACK_BOTTOM;
    size_t length = strnlen(str, available);

    return stack_push(mm, sp, str, length + 1);
}

static int setup_stack(struct mm_struct *mm, const struct exec_image *image,
                       const char *path, const char *const argv[],
                       const char *const envp[], vaddr_t *user_sp)
{
    struct vm_area_struct *vma __free(vma) = vma_alloc();
    size_t argc = 0, envc = 0;
    vaddr_t sp = USTACK_TOP, execfn, random_addr, phdr_addr = image->phdr_addr;
    size_t phsize = (size_t)image->ehdr.e_phnum * sizeof(*image->phdrs);
    int ret;

    if (!vma)
        return -ENOMEM;
    vma->vm_start = USTACK_BOTTOM;
    vma->vm_end = USTACK_TOP;
    vma->vm_flags |= VM_READ | VM_WRITE;
    ret = vma_insert(mm, vma);
    if (ret)
        return ret;
    keep(vma);

    while (argv[argc])
        ++argc;
    while (envp[envc])
        ++envc;
    if (!phdr_addr) {
        ret = stack_push(mm, &sp, image->phdrs, phsize);
        if (ret)
            return ret;
        phdr_addr = sp;
    }
    ret = stack_string(mm, &sp, path);
    if (ret)
        return ret;
    execfn = sp;
    ret = stack_push(mm, &sp, initial_random, sizeof(initial_random));
    if (ret)
        return ret;
    random_addr = sp;

    const unsigned long auxv[] = {
        AT_PHDR,   phdr_addr,
        AT_PHENT,  sizeof(*image->phdrs),
        AT_PHNUM,  image->ehdr.e_phnum,
        AT_PAGESZ, PAGE_SIZE,
        AT_BASE,   0,
        AT_ENTRY,  image->ehdr.e_entry,
        AT_UID,    0,
        AT_EUID,   0,
        AT_GID,    0,
        AT_EGID,   0,
        AT_HWCAP,  ELF_HWCAP,
        AT_SECURE, 0,
        AT_RANDOM, random_addr,
        AT_EXECFN, execfn,
        AT_NULL,   0,
    };
    size_t words = 1 + argc + 1 + envc + 1 + ARRLEN(auxv);

    if (words > USTACK_SIZE / sizeof(unsigned long))
        return -E2BIG;
    unsigned long *table __free(kfree) = kzalloc(words * sizeof(*table));

    if (!table)
        return -ENOMEM;
    table[0] = argc;
    for (size_t i = 0; i < argc; ++i) {
        ret = stack_string(mm, &sp, argv[i]);
        if (ret)
            return ret;
        table[1 + i] = sp;
    }
    for (size_t i = 0; i < envc; ++i) {
        ret = stack_string(mm, &sp, envp[i]);
        if (ret)
            return ret;
        table[1 + argc + 1 + i] = sp;
    }
    memcpy(table + 1 + argc + 1 + envc + 1, auxv, sizeof(auxv));
    size_t bytes = words * sizeof(*table);

    if (bytes > sp - USTACK_BOTTOM)
        return -E2BIG;
    sp = ALIGN_DOWN(sp - bytes, (vaddr_t)USTACK_ALIGN);
    ret = stack_copy(mm, sp, table, bytes);
    if (ret)
        return ret;
    *user_sp = sp;
    return 0;
}

static int load_exec(struct task *task, struct file *file, const char *path,
                     const char *const argv[], const char *const envp[])
{
    struct elf64_phdr *phdrs __free(kfree) = NULL;
    struct exec_image image = {0};
    vaddr_t sp;
    int ret;

    ret = read_exact(file, &image.ehdr, sizeof(image.ehdr), 0);
    if (ret)
        return ret;

    ret = elf_check_ehdr(&image.ehdr, INT64_MAX);
    if (ret)
        return ret;
    if (image.ehdr.e_type != ET_EXEC)
        return -ENOEXEC;

    size_t phsize = (size_t)image.ehdr.e_phnum * sizeof(*image.phdrs);
    phdrs = kmalloc(phsize);
    if (!phdrs)
        return -ENOMEM;

    ret = read_exact(file, phdrs, phsize, image.ehdr.e_phoff);
    if (ret)
        return ret;

    ret = elf_check_phdrs(&image.ehdr, phdrs, INT64_MAX);
    if (ret)
        return ret;
    image.phdrs = phdrs;

    ret = check_layout(&image);
    if (ret)
        return ret;

    mutex_lock(&task->mm->mmap_lock);

    ret = load_segments(task->mm, file, &image);
    if (ret)
        goto out;

    ret = setup_stack(task->mm, &image, path, argv, envp, &sp);
    if (ret)
        goto out;

    ret = arch_utask_init(task, image.ehdr.e_entry, sp);

out:
    mutex_unlock(&task->mm->mmap_lock);
    return ret;
}

int execve(const char *path, const char *const argv[], const char *const envp[])
{
    struct task *task __free(task) = NULL;
    struct file *file __free(fput) = NULL;
    int ret;

    task = task_alloc(true);
    if (!task)
        return -ENOMEM;

    ret = pid_prepare(task, NULL);
    if (ret)
        return ret;

    file = vfs_open(path, O_RDONLY);
    if (IS_ERR(file))
        return PTR_ERR(move(file));

    ret = load_exec(task, file, path, argv, envp);
    if (ret)
        return ret;

    task->did_exec = true;
    sched_publish(task);
    task_put(move(task));
    return 0;
}
