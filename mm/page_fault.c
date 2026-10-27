#include <nuvix/mm.h>
#include <nuvix/page.h>
#include <nuvix/pgtable.h>
#include <nuvix/errno.h>
#include <nuvix/math.h>
#include <nuvix/string.h>
#include <nuvix/vfs.h>
#include <nuvix/tlb.h>

int handle_page_fault(struct mm_struct *mm, vaddr_t addr, unsigned long access)
{
    struct page *page __free(page) = NULL;
    struct page *old __free(page) = NULL;
    struct vm_struct *vma;
    paddr_t paddr;
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
    ret = pgtable_lookup(mm, addr, &paddr);
    if (!ret && !(access & VM_WRITE))
        goto out;
    Assert(!ret || ret == -ENOENT, "fault requires a valid user address\n");
    if (!ret && refcount_read(&phys_to_page(paddr)->refcount) == 1) {
        pgtable_protect_page(mm, addr, vma->vm_flags);
        flush_tlb_all();
        goto out;
    }
    page = alloc_page();
    if (!page) {
        ret = -ENOMEM;
        goto out;
    }
    if (!ret) {
        memcpy((void *)__va(page_to_phys(page)), (void *)__va(paddr),
               PAGE_SIZE);
        old = pgtable_unmap_page(mm, addr);
        ret = pgtable_map_page(mm, addr, page, vma->vm_flags);
        Assert(!ret, "COW remap requires existing page tables\n");
        keep(page);
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
