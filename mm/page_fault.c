#include <nuvix/mm.h>
#include <nuvix/page.h>
#include <nuvix/pgtable.h>
#include <nuvix/errno.h>
#include <nuvix/math.h>
#include <nuvix/string.h>
#include <nuvix/vfs.h>

int handle_page_fault(struct mm_struct *mm, vaddr_t addr, unsigned long access)
{
    struct page *page __free(page) = NULL;
    struct vm_struct *vma;
    int ret;

    mutex_lock(&mm->mmap_lock);
    vma = vma_find(mm, addr);
    if (!vma) {
        ret = -EFAULT;
        goto out;
    }
    if (!(vma->vm_flags & access)) {
        ret = -EACCES;
        goto out;
    }
    addr = ALIGN_DOWN(addr, (vaddr_t)PAGE_SIZE);
    page = alloc_page();
    if (!page) {
        ret = -ENOMEM;
        goto out;
    }
    memset((void *)__va(page_to_phys(page)), 0, PAGE_SIZE);
    if (vma->vm_file) {
        vaddr_t first = MAX(addr, vma->vm_fstart);
        vaddr_t last = MIN(addr + PAGE_SIZE, vma->vm_fend);
        loff_t pos = vma->vm_offset + first - vma->vm_start;
        char *data = (char *)__va(page_to_phys(page));

        while (first < last) {
            ssize_t count = vma->vm_file->f_ops->read(
                vma->vm_file, data + first - addr, last - first, &pos);

            if (count <= 0) {
                ret = count ? (int)count : -EIO;
                goto out;
            }
            first += count;
        }
    }
    ret = pgtable_map_page(mm, addr, page, vma->vm_flags);
    if (!ret)
        keep(page);
out:
    mutex_unlock(&mm->mmap_lock);
    return ret;
}
