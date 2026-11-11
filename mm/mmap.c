#include <nuvix/mm.h>
#include <nuvix/math.h>
#include <nuvix/layout.h>
#include <nuvix/errno.h>
#include <nuvix/syscall.h>
#include <nuvix/task.h>
#include <nuvix/vfs.h>
#include <nuvix/fdtable.h>
#include <asm/trap.h>

static unsigned long vm_prot(unsigned long prot)
{
    unsigned long flags = 0;

    if (prot & PROT_READ)
        flags |= VM_READ;
    if (prot & PROT_WRITE)
        flags |= VM_READ | VM_WRITE;
    if (prot & PROT_EXEC)
        flags |= VM_EXEC;
    return flags;
}

static bool range_allowed(vaddr_t start, vaddr_t end)
{
    if (start < UVADDR_START || end > USTACK_TOP)
        return false;
    return end <= USTACK_BOTTOM - PAGE_SIZE || start >= USTACK_BOTTOM;
}

static int range_end(vaddr_t start, size_t len, vaddr_t *end)
{
    if (start & (PAGE_SIZE - 1))
        return -EINVAL;
    if (!len)
        return -EINVAL;
    if (len > SIZE_MAX - (PAGE_SIZE - 1))
        return -ENOMEM;
    len = ALIGN_UP(len, (size_t)PAGE_SIZE);
    if (start >= UVADDR_END || len > UVADDR_END - start)
        return -ENOMEM;
    *end = start + len;
    if (!range_allowed(start, *end))
        return -EINVAL;
    return 0;
}

static vaddr_t find_unmapped(struct mm_struct *mm, vaddr_t hint, size_t len)
{
    vaddr_t top = USTACK_BOTTOM - PAGE_SIZE;
    struct rb_node *node;

    hint = ALIGN_DOWN(hint, (vaddr_t)PAGE_SIZE);
    if (hint < UVADDR_END && len <= UVADDR_END - hint &&
        range_allowed(hint, hint + len) &&
        !vma_find_intersection(mm, hint, hint + len))
        return hint;

    for (node = rb_last(&mm->vma_tree); node; node = rb_prev(node)) {
        struct vm_struct *vma = rb_entry(node, struct vm_struct, vm_node);

        if (vma->vm_start >= top)
            continue;
        if (vma->vm_end <= top && len <= top - vma->vm_end)
            return top - len;
        top = vma->vm_start;
    }
    return len <= top - UVADDR_START ? top - len : 0;
}

long sys_mmap(struct trap_frame *frame)
{
    vaddr_t addr = syscallarg(frame, 0);
    size_t len = syscallarg(frame, 1);
    unsigned long prot = syscallarg(frame, 2);
    unsigned long flags = syscallarg(frame, 3);
    unsigned long offset = syscallarg(frame, 5);
    struct mm_struct *mm = current->mm;
    struct file *file __free(fput) = NULL;
    struct vm_struct *vma __free(vma) = NULL;
    vaddr_t end;
    long ret;

    if ((flags & ~(MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED)) ||
        !(flags & MAP_PRIVATE) ||
        (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) ||
        (offset & (PAGE_SIZE - 1)) || !len)
        return -EINVAL;
    if (len > UVADDR_SIZE)
        return -ENOMEM;
    len = ALIGN_UP(len, (size_t)PAGE_SIZE);

    if (!(flags & MAP_ANONYMOUS)) {
        file = fd_open(current->fdtable, syscallarg(frame, 4));
        if (!file)
            return -EBADF;
        if (!FILE_READ(file))
            return -EACCES;
        if (!S_ISREG(file->f_inode->i_mode))
            return -ENODEV;
        if (offset > INT64_MAX || len > INT64_MAX - offset)
            return -EOVERFLOW;
    }

    vma = vma_alloc();
    if (!vma)
        return -ENOMEM;
    mutex_lock(&mm->mmap_lock);
    if (flags & MAP_FIXED) {
        ret = range_end(addr, len, &end);
        if (ret)
            goto out;
        ret = unmap_range(mm, addr, end);
        if (ret)
            goto out;
    } else {
        addr = find_unmapped(mm, addr, len);
        if (!addr) {
            ret = -ENOMEM;
            goto out;
        }
        end = addr + len;
    }
    vma->vm_start = addr;
    vma->vm_end = end;
    vma->vm_flags = vm_prot(prot);
    if (file) {
        vma->vm_file = move(file);
        vma->vm_offset = offset;
        vma->vm_fstart = addr;
        vma->vm_fend = end;
    }
    ret = vma_insert(mm, vma);
    Assert(!ret, "selected mmap range must be free\n");
    keep(vma);
    ret = addr;
out:
    mutex_unlock(&mm->mmap_lock);
    return ret;
}

long sys_mremap(struct trap_frame *frame)
{
    vaddr_t addr = syscallarg(frame, 0);
    size_t olen = syscallarg(frame, 1);
    size_t nlen = syscallarg(frame, 2);
    unsigned long flags = syscallarg(frame, 3);
    struct mm_struct *mm = current->mm;
    struct vm_struct *vma;
    vaddr_t oend, nend, naddr;
    long ret;

    if (flags & ~MREMAP_MAYMOVE)
        return -EINVAL;
    ret = range_end(addr, olen, &oend);
    if (ret)
        return ret;
    if (!nlen)
        return -EINVAL;
    if (nlen > UVADDR_SIZE)
        return -ENOMEM;
    olen = oend - addr;
    nlen = ALIGN_UP(nlen, (size_t)PAGE_SIZE);

    mutex_lock(&mm->mmap_lock);
    vma = vma_find(mm, addr);
    if (!vma || oend > vma->vm_end) {
        ret = -EFAULT;
        goto out;
    }
    if (vma->vm_file) {
        ret = -EOPNOTSUPP;
        goto out;
    }
    if (nlen == olen) {
        ret = addr;
        goto out;
    }
    if (nlen < olen) {
        ret = unmap_range(mm, addr + nlen, oend);
        if (!ret)
            ret = addr;
        goto out;
    }
    ret = range_end(addr, nlen, &nend);
    if (!ret && oend == vma->vm_end && !vma_find_intersection(mm, oend, nend)) {
        vma_remove(vma);
        vma->vm_end = nend;
        ret = vma_insert(mm, vma);
        Assert(!ret, "failed to insert expanded VMA\n");
        ret = addr;
        goto out;
    }
    ret = -ENOMEM;
    if (!(flags & MREMAP_MAYMOVE))
        goto out;
    naddr = find_unmapped(mm, 0, nlen);
    if (!naddr)
        goto out;
    ret = move_range(mm, addr, oend, naddr, naddr + nlen);
    if (!ret)
        ret = naddr;
out:
    mutex_unlock(&mm->mmap_lock);
    return ret;
}

long sys_munmap(struct trap_frame *frame)
{
    vaddr_t start = syscallarg(frame, 0);
    size_t len = syscallarg(frame, 1);
    struct mm_struct *mm = current->mm;
    vaddr_t end;
    int ret = range_end(start, len, &end);

    if (ret)
        return ret;
    mutex_lock(&mm->mmap_lock);
    ret = unmap_range(mm, start, end);
    mutex_unlock(&mm->mmap_lock);
    return ret;
}

long sys_mprotect(struct trap_frame *frame)
{
    vaddr_t start = syscallarg(frame, 0);
    size_t len = syscallarg(frame, 1);
    unsigned long prot = syscallarg(frame, 2);
    struct mm_struct *mm = current->mm;
    vaddr_t end;
    int ret;

    if (start & (PAGE_SIZE - 1))
        return -EINVAL;
    if (!len)
        return 0;
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
        return -EINVAL;
    ret = range_end(start, len, &end);
    if (ret)
        return ret;
    mutex_lock(&mm->mmap_lock);
    ret = protect_range(mm, start, end, vm_prot(prot));
    mutex_unlock(&mm->mmap_lock);
    return ret;
}

long sys_brk(struct trap_frame *frame)
{
    vaddr_t addr = syscallarg(frame, 0);
    struct mm_struct *mm = current->mm;
    vaddr_t old, start, end;

    mutex_lock(&mm->mmap_lock);
    old = mm->brk;
    if (addr < mm->start_brk || addr > USTACK_BOTTOM - PAGE_SIZE)
        goto out;
    start = ALIGN_UP(old, (vaddr_t)PAGE_SIZE);
    end = ALIGN_UP(addr, (vaddr_t)PAGE_SIZE);
    if (end > start) {
        struct vm_struct *vma;

        if (vma_find_intersection(mm, start, end))
            goto out;
        vma = map_range(mm, start, end, VM_READ | VM_WRITE);
        if (IS_ERR(vma))
            goto out;
    } else if (end < start) {
        if (unmap_range(mm, end, start))
            goto out;
    }
    mm->brk = addr;
out:
    old = mm->brk;
    mutex_unlock(&mm->mmap_lock);
    return old;
}
