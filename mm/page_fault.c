#include <nuvix/mm.h>
#include <nuvix/page.h>
#include <nuvix/pgtable.h>
#include <nuvix/errno.h>
#include <nuvix/math.h>
#include <nuvix/string.h>

int handle_page_fault(struct mm_struct *mm, vaddr_t addr, unsigned long access)
{
    struct page *page __free(page) = NULL;
    struct vm_area_struct *vma;
    size_t index;
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
    index = (addr - vma->vm_start) >> PAGE_SHIFT;
    if (index < vma->vm_nr_pages) {
        ret = pgtable_map_page(mm, addr, vma->vm_pages[index], vma->vm_flags);
        if (!ret)
            vma->vm_pages[index] = NULL;
        goto out;
    }

    page = alloc_page();
    if (!page) {
        ret = -ENOMEM;
        goto out;
    }
    memset((void *)__va(page_to_phys(page)), 0, PAGE_SIZE);
    ret = pgtable_map_page(mm, addr, page, vma->vm_flags);
    if (!ret)
        keep(page);
out:
    mutex_unlock(&mm->mmap_lock);
    return ret;
}
