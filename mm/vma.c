#include <nuvix/mm.h>
#include <nuvix/layout.h>
#include <nuvix/slab.h>
#include <nuvix/errno.h>
#include <nuvix/assert.h>
#include <nuvix/page.h>
#include <nuvix/vfs.h>

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

