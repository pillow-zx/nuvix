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
#include <nuvix/fdtable.h>
#include <nuvix/sched.h>
#include <nuvix/tty.h>
#include <nuvix/uaccess.h>
#include <nuvix/syscall.h>
#include <asm/trap.h>
#include <asm/elf.h>

#define ELF_MAGIC_NUMBER 0x9e3779b97f4a7c15ULL

struct task *init;

static const uint64_t initial_random[ELF_RANDOM_SIZE / sizeof(uint64_t)] = {
    ELF_MAGIC_NUMBER, ELF_MAGIC_NUMBER};

struct exec_params {
    struct mm_struct *mm;
    const char *path;
    const char *const *argv;
    const char *const *envp;
    vaddr_t sp;
};

struct exec_image {
    struct elf64_ehdr ehdr;
    struct elf64_phdr *phdrs;
    vaddr_t offset;
    vaddr_t phdr_addr;
};

static int read_exact(struct file *file, void *buffer, size_t size,
                      uint64_t offset)
{
    struct kiocb iocb = {.file = file, .pos = (loff_t)offset};
    struct iovec vec = {.iov_base = buffer, .iov_len = size};
    struct kiov iov;

    kiov_init(&iov, &vec, 1, size, false);
    while (iov.count) {
        ssize_t ret = vfs_read(&iocb, &iov);

        if (ret < 0)
            return (int)ret;
        if (!ret)
            return -ENOEXEC;
    }
    return 0;
}

static int check_layout(struct exec_image *image)
{
    size_t phsize = (size_t)image->ehdr.e_phnum * sizeof(*image->phdrs);
    vaddr_t load_end = UVADDR_START;

    for (unsigned int i = 0; i < image->ehdr.e_phnum; ++i) {
        const struct elf64_phdr *phdr = &image->phdrs[i];
        vaddr_t start, end;

        if (phdr->p_type != PT_LOAD || !phdr->p_memsz)
            continue;
        if (phdr->p_vaddr < UVADDR_START - image->offset ||
            phdr->p_vaddr + phdr->p_memsz >
                USTACK_BOTTOM - PAGE_SIZE - image->offset ||
            ((phdr->p_vaddr ^ phdr->p_offset) & (PAGE_SIZE - 1)))
            return -ENOEXEC;
        start = ALIGN_DOWN(image->offset + phdr->p_vaddr, (vaddr_t)PAGE_SIZE);
        end = ALIGN_UP(image->offset + phdr->p_vaddr + phdr->p_memsz,
                       (vaddr_t)PAGE_SIZE);
        if (start < load_end)
            return -ENOEXEC;
        load_end = end;
        if ((phdr->p_flags & PF_R) && image->ehdr.e_phoff >= phdr->p_offset &&
            image->ehdr.e_phoff - phdr->p_offset <= phdr->p_filesz &&
            phsize <= phdr->p_filesz - (image->ehdr.e_phoff - phdr->p_offset))
            image->phdr_addr = image->offset + phdr->p_vaddr +
                               (image->ehdr.e_phoff - phdr->p_offset);
    }

    return 0;
}

static int load_segments(struct mm_struct *mm, struct file *file,
                         const struct exec_image *image)
{
    for (unsigned int i = 0; i < image->ehdr.e_phnum; ++i) {
        const struct elf64_phdr *phdr = &image->phdrs[i];
        struct vm_struct *vma;
        unsigned long flags = 0;
        vaddr_t vaddr, start, end;

        if (phdr->p_type != PT_LOAD || !phdr->p_memsz)
            continue;
        vaddr = image->offset + phdr->p_vaddr;
        if (phdr->p_flags & PF_R)
            flags |= VM_READ;
        if (phdr->p_flags & PF_W)
            flags |= VM_READ | VM_WRITE;
        if (phdr->p_flags & PF_X)
            flags |= VM_EXEC;
        start = ALIGN_DOWN(vaddr, (vaddr_t)PAGE_SIZE);
        end = ALIGN_UP(vaddr + phdr->p_memsz, (vaddr_t)PAGE_SIZE);

        vma = map_range(mm, start, end, flags);
        if (IS_ERR(vma))
            return PTR_ERR(vma);
        if (phdr->p_filesz) {
            vma->vm_file = fget(file);
            vma->vm_offset = phdr->p_offset + start - vaddr;
            vma->vm_fstart = vaddr;
            vma->vm_fend = vaddr + phdr->p_filesz;
        }
        mm->start_brk = MAX(mm->start_brk, end);
    }
    mm->brk = mm->start_brk;
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
                                   page, VM_READ | VM_WRITE);
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

static int stack_push(struct exec_params *params, const void *src, size_t size)
{
    if (size > params->sp - USTACK_BOTTOM)
        return -E2BIG;
    params->sp -= size;
    return stack_copy(params->mm, params->sp, src, size);
}

static int setup_stack(struct exec_params *params,
                       const struct exec_image *image, vaddr_t entry)
{
    struct vm_struct *vma =
        map_range(params->mm, USTACK_BOTTOM, USTACK_TOP, VM_READ | VM_WRITE);
    size_t argc = 0, envc = 0;
    vaddr_t execfn, random_addr, phdr_addr = image->phdr_addr;
    size_t phsize = (size_t)image->ehdr.e_phnum * sizeof(*image->phdrs);
    int ret;

    if (IS_ERR(vma))
        return PTR_ERR(vma);

    params->sp = USTACK_TOP;
    while (params->argv[argc])
        ++argc;
    while (params->envp[envc])
        ++envc;
    if (!phdr_addr) {
        ret = stack_push(params, image->phdrs, phsize);
        if (ret)
            return ret;
        phdr_addr = params->sp;
    }
    ret = stack_push(params, params->path, strlen(params->path) + 1);
    if (ret)
        return ret;
    execfn = params->sp;
    ret = stack_push(params, initial_random, sizeof(initial_random));
    if (ret)
        return ret;
    random_addr = params->sp;

    const unsigned long auxv[] = {
        AT_PHDR,   phdr_addr,
        AT_PHENT,  sizeof(*image->phdrs),
        AT_PHNUM,  image->ehdr.e_phnum,
        AT_PAGESZ, PAGE_SIZE,
        AT_BASE,   0,
        AT_ENTRY,  entry,
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
        ret = stack_push(params, params->argv[i], strlen(params->argv[i]) + 1);
        if (ret)
            return ret;
        table[1 + i] = params->sp;
    }
    for (size_t i = 0; i < envc; ++i) {
        ret = stack_push(params, params->envp[i], strlen(params->envp[i]) + 1);
        if (ret)
            return ret;
        table[1 + argc + 1 + i] = params->sp;
    }
    memcpy(table + 1 + argc + 1 + envc + 1, auxv, sizeof(auxv));
    size_t bytes = words * sizeof(*table);

    if (bytes > params->sp - USTACK_BOTTOM)
        return -E2BIG;
    params->sp = ALIGN_DOWN(params->sp - bytes, (vaddr_t)USTACK_ALIGN);
    return stack_copy(params->mm, params->sp, table, bytes);
}

static int load_exec(struct exec_params *params, vaddr_t *entry)
{
    struct file *file __free(fput) = NULL;
    struct elf64_phdr *phdrs __free(kfree) = NULL;
    struct exec_image image = {0};
    vaddr_t elf_entry;
    int ret;

    file = vfs_open(AT_FDCWD, params->path, O_RDONLY);
    if (IS_ERR(file))
        return PTR_ERR(move(file));

    ret = read_exact(file, &image.ehdr, sizeof(image.ehdr), 0);
    if (ret)
        return ret;

    ret = elf_check_ehdr(&image.ehdr, INT64_MAX);
    if (ret)
        return ret;

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
    image.offset = image.ehdr.e_type == ET_DYN ? UVADDR_START : 0;

    ret = check_layout(&image);
    if (ret)
        return ret;

    mutex_lock(&params->mm->mmap_lock);

    ret = load_segments(params->mm, file, &image);
    if (ret)
        goto out;

    elf_entry = image.offset + image.ehdr.e_entry;
    ret = setup_stack(params, &image, elf_entry);
    if (ret)
        goto out;

    *entry = elf_entry;

out:
    mutex_unlock(&params->mm->mmap_lock);
    return ret;
}

int init_execve(const char *path, const char *const argv[],
                const char *const envp[])
{
    struct task *task __free(task) = NULL;
    struct exec_params params = {.path = path, .argv = argv, .envp = envp};
    vaddr_t entry;
    int ret;

    task = task_alloc(true);
    if (!task)
        return -ENOMEM;

    ret = pid_prepare(task, NULL);
    if (ret)
        return ret;

    ret = vfs_get_root(&task->cwd);
    if (ret)
        return ret;

    params.mm = task->mm;
    ret = load_exec(&params, &entry);
    if (ret)
        return ret;

    ret = arch_utask_init(task, task->mm, entry, params.sp);
    if (ret)
        return ret;

    task_clear_flag(task, TF_FORKNOEXEC);
    ret = init_stdio(task->fdtable);
    if (ret)
        return ret;

    init = move(task);
    sched_publish(init);
    return 0;
}

long sys_execve(struct trap_frame *frame)
{
    const char *upath = (const char *)syscallarg(frame, 0);
    const char *const *uargv = (const void *)syscallarg(frame, 1);
    const char *const *uenvp = (const void *)syscallarg(frame, 2);
    char *path __free(kfree) = get_pathname(upath);
    char *strings __free(kfree) = NULL;
    const char **pointers __free(kfree) = NULL;
    struct mm_struct *mm __free(mm_put) = NULL;
    struct exec_params params = {.path = path};
    size_t nr_pointers = 0;
    size_t string_bytes = 0;
    size_t env_start;
    vaddr_t entry;
    long ret;

    if (IS_ERR(path))
        return PTR_ERR(move(path));
    strings = kmalloc(USTACK_SIZE);
    pointers = kmalloc(USTACK_SIZE);
    if (!strings || !pointers)
        return -ENOMEM;

    for (;;) {
        const char *str = NULL;
        size_t capacity = USTACK_SIZE - string_bytes;
        ssize_t length;

        if (nr_pointers == USTACK_SIZE / sizeof(*pointers))
            return -E2BIG;
        if (uargv && copy_from_user(&str, uargv++, sizeof(str)))
            return -EFAULT;
        pointers[nr_pointers++] = NULL;
        if (!str)
            break;
        length = strncpy_from_user(strings + string_bytes, str, capacity);
        if (length < 0)
            return length;
        if ((size_t)length == capacity)
            return -E2BIG;
        pointers[nr_pointers - 1] = strings + string_bytes;
        string_bytes += length + 1;
    }

    if (nr_pointers == 1) {
        pointers[0] = "";
        pointers[1] = NULL;
        nr_pointers = 2;
    }
    env_start = nr_pointers;

    for (;;) {
        const char *str = NULL;
        size_t capacity = USTACK_SIZE - string_bytes;
        ssize_t length;

        if (nr_pointers == USTACK_SIZE / sizeof(*pointers))
            return -E2BIG;
        if (uenvp && copy_from_user(&str, uenvp++, sizeof(str)))
            return -EFAULT;
        pointers[nr_pointers++] = NULL;
        if (!str)
            break;
        length = strncpy_from_user(strings + string_bytes, str, capacity);
        if (length < 0)
            return length;
        if ((size_t)length == capacity)
            return -E2BIG;
        pointers[nr_pointers - 1] = strings + string_bytes;
        string_bytes += length + 1;
    }
    mm = mm_alloc();
    if (!mm)
        return -ENOMEM;

    params.mm = mm;
    params.argv = pointers;
    params.envp = pointers + env_start;
    ret = load_exec(&params, &entry);
    if (ret)
        return ret;

    ret = arch_utask_init(current, mm, entry, params.sp);
    if (ret)
        return ret;

    signal_clone();
    task_clear_flag(current, TF_FORKNOEXEC);
    fdtable_close_on_exec(current->fdtable);
    switch_mm(move(mm));
    task_vfork_done(current);
    return 0;
}
