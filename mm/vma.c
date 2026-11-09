#include <nuvix/mm.h>
#include <nuvix/layout.h>
#include <nuvix/slab.h>
#include <nuvix/errno.h>
#include <nuvix/assert.h>
#include <nuvix/page.h>
#include <nuvix/pgtable.h>
#include <nuvix/tlb.h>
#include <nuvix/vfs.h>
#include <nuvix/math.h>

#define UNMAP_BATCH_PAGES 32

struct unmaps {
    struct page *pages[UNMAP_BATCH_PAGES];
    size_t nr_pages;
};

static void unmaps_flush(struct unmaps *batch)
{
    if (!batch->nr_pages)
        return;
    flush_tlb_all();
    for (size_t i = 0; i < batch->nr_pages; ++i)
        free_page(batch->pages[i]);
    batch->nr_pages = 0;
}

struct vm_struct *vma_alloc(void)
{
    struct vm_struct *vma __free(kfree) = kzalloc(sizeof(*vma));

    if (!vma)
        return NULL;
    RB_CLEAR_NODE(&vma->vm_node);
    return move(vma);
}

void vma_free(struct vm_struct *vma)
{
    Assert(!vma->vm_mm && RB_EMPTY_NODE(&vma->vm_node),
           "VMA free requires a detached VMA\n");
    fput(vma->vm_file);
    kfree(vma);
}

struct vm_struct *vma_find(struct mm_struct *mm, vaddr_t addr)
{
    struct rb_node *node = mm->vma_tree.rb_node;

    while (node) {
        struct vm_struct *vma = rb_entry(node, struct vm_struct, vm_node);

        if (addr < vma->vm_start)
            node = node->rb_left;
        else if (addr >= vma->vm_end)
            node = node->rb_right;
        else
            return vma;
    }
    return NULL;
}

struct vm_struct *vma_find_intersection(struct mm_struct *mm, vaddr_t start,
                                        vaddr_t end)
{
    struct rb_node *node = mm->vma_tree.rb_node;
    struct vm_struct *found = NULL;

    while (node) {
        struct vm_struct *vma = rb_entry(node, struct vm_struct, vm_node);

        if (start >= vma->vm_end) {
            node = node->rb_right;
        } else {
            if (vma->vm_start < end)
                found = vma;
            node = node->rb_left;
        }
    }
    return found;
}

int vma_insert(struct mm_struct *mm, struct vm_struct *vma)
{
    struct rb_node **link = &mm->vma_tree.rb_node;
    struct rb_node *parent = NULL;

    Assert(!vma->vm_mm && RB_EMPTY_NODE(&vma->vm_node),
           "VMA insert requires a detached VMA\n");
    if (vma->vm_start < UVADDR_START || vma->vm_end > UVADDR_END ||
        vma->vm_start >= vma->vm_end ||
        ((vma->vm_start | vma->vm_end) & (PAGE_SIZE - 1)) ||
        (vma->vm_flags & ~(VM_READ | VM_WRITE | VM_EXEC)))
        return -EINVAL;

    while (*link) {
        struct vm_struct *queued = rb_entry(*link, struct vm_struct, vm_node);

        parent = *link;
        if (vma->vm_end <= queued->vm_start)
            link = &parent->rb_left;
        else if (vma->vm_start >= queued->vm_end)
            link = &parent->rb_right;
        else
            return -EEXIST;
    }
    vma->vm_mm = mm;
    rb_link_node(&vma->vm_node, parent, link);
    rb_insert_color(&vma->vm_node, &mm->vma_tree);
    return 0;
}

void vma_remove(struct vm_struct *vma)
{
    Assert(vma->vm_mm && !RB_EMPTY_NODE(&vma->vm_node),
           "VMA remove requires a linked VMA\n");
    rb_erase(&vma->vm_node, &vma->vm_mm->vma_tree);
    RB_CLEAR_NODE(&vma->vm_node);
    vma->vm_mm = NULL;
}

struct vm_struct *map_range(struct mm_struct *mm, vaddr_t start, vaddr_t end,
                            unsigned long vm_flags)
{
    struct vm_struct *vma __free(vma) = vma_alloc();
    int ret;

    if (!vma)
        return ERR_PTR(-ENOMEM);
    vma->vm_start = start;
    vma->vm_end = end;
    vma->vm_flags = vm_flags;
    ret = vma_insert(mm, vma);
    if (ret)
        return ERR_PTR(ret);
    return move(vma);
}

static void vma_split(struct vm_struct *vma, vaddr_t addr,
                      struct vm_struct *right)
{
    struct mm_struct *mm = vma->vm_mm;
    int ret;

    *right = *vma;
    right->vm_mm = NULL;
    RB_CLEAR_NODE(&right->vm_node);
    right->vm_start = addr;
    if (right->vm_file) {
        fget(right->vm_file);
        right->vm_offset += addr - vma->vm_start;
        right->vm_fstart = MAX(addr, right->vm_fstart);
        right->vm_fend = MAX(addr, right->vm_fend);
    }
    vma_remove(vma);
    vma->vm_end = addr;
    if (vma->vm_file) {
        vma->vm_fstart = MIN(addr, vma->vm_fstart);
        vma->vm_fend = MIN(addr, vma->vm_fend);
    }
    ret = vma_insert(mm, vma);
    Assert(!ret, "failed to reinsert split VMA\n");
    ret = vma_insert(mm, right);
    Assert(!ret, "failed to insert split VMA\n");
}

static int split_range(struct mm_struct *mm, vaddr_t start, vaddr_t end)
{
    struct vm_struct *left __free(vma) = NULL;
    struct vm_struct *right __free(vma) = NULL;
    struct vm_struct *first = vma_find(mm, start);
    struct vm_struct *last = vma_find(mm, end - 1);

    Assert(mm->mmap_lock.owner == current && start < end &&
               !((start | end) & (PAGE_SIZE - 1)),
           "range modification requires locked, aligned bounds\n");
    if (first && first->vm_start < start) {
        left = vma_alloc();
        if (!left)
            return -ENOMEM;
    }
    if (last && end < last->vm_end) {
        right = vma_alloc();
        if (!right)
            return -ENOMEM;
    }
    if (left)
        vma_split(first, start, move(left));
    if (right)
        vma_split(vma_find(mm, end - 1), end, move(right));
    return 0;
}

int unmap_range(struct mm_struct *mm, vaddr_t start, vaddr_t end)
{
    struct unmaps batch = {0};
    struct vm_struct *found;
    int ret = split_range(mm, start, end);

    if (ret)
        return ret;
    while ((found = vma_find_intersection(mm, start, end))) {
        struct vm_struct *vma __free(vma) = found;

        vma_remove(vma);
        for (vaddr_t addr = vma->vm_start; addr < vma->vm_end;
             addr += PAGE_SIZE) {
            struct page *page = pgtable_unmap_page(mm, addr);

            if (!page)
                continue;
            batch.pages[batch.nr_pages++] = page;
            if (batch.nr_pages == UNMAP_BATCH_PAGES)
                unmaps_flush(&batch);
        }
    }
    unmaps_flush(&batch);
    return 0;
}

int move_range(struct mm_struct *mm, vaddr_t start, vaddr_t end, vaddr_t nstart,
               vaddr_t nend)
{
    struct vm_struct *vma = vma_find(mm, start);
    struct vm_struct *left __free(vma) = NULL;
    struct vm_struct *right __free(vma) = NULL;
    size_t len = end - start;
    int ret;

    Assert(mm->mmap_lock.owner == current && vma && !vma->vm_file &&
               start < end && end <= vma->vm_end &&
               nend - nstart >= end - start,
           "range move requires a locked anonymous VMA and sufficient space\n");
    if (vma->vm_start < start) {
        left = vma_alloc();
        if (!left)
            return -ENOMEM;
    }
    if (end < vma->vm_end) {
        right = vma_alloc();
        if (!right)
            return -ENOMEM;
    }
    ret = pgtable_move(mm, start, nstart, len);
    if (ret)
        return ret;
    if (left) {
        vma_split(vma, start, move(left));
        vma = vma_find(mm, start);
    }
    if (right)
        vma_split(vma, end, move(right));
    vma_remove(vma);
    vma->vm_start = nstart;
    vma->vm_end = nend;
    ret = vma_insert(mm, vma);
    Assert(!ret, "failed to insert moved VMA\n");
    return 0;
}

int protect_range(struct mm_struct *mm, vaddr_t start, vaddr_t end,
                  unsigned long vm_flags)
{
    bool changed = false;
    int ret = split_range(mm, start, end);

    if (ret)
        return ret;
    if (vm_flags & VM_EXEC)
        flush_icache_all();
    while (start < end) {
        struct vm_struct *vma = vma_find(mm, start);

        if (!vma) {
            ret = -ENOMEM;
            break;
        }
        vma->vm_flags = vm_flags;
        for (; start < vma->vm_end; start += PAGE_SIZE)
            changed |= pgtable_protect_page(mm, start, vm_flags);
    }
    if (changed)
        flush_tlb_all();
    return ret;
}
