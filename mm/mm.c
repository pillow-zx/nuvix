#include <nuvix/mm.h>
#include <nuvix/pgtable.h>
#include <nuvix/slab.h>
#include <nuvix/vfs.h>
#include <nuvix/errno.h>

struct mm_struct *mm_alloc(void)
{
    struct mm_struct *mm __free(kfree) = kzalloc(sizeof(*mm));
    if (!mm)
        return NULL;

    paddr_t pgd __free(pgd) = 0;
    int ret = alloc_pgd(&pgd);
    if (ret)
        return NULL;
    refcount_set(&mm->urefs, 1);
    refcount_set(&mm->refs, 1);
    mutex_init(&mm->mmap_lock);
    mm->vma_tree = RB_ROOT;
    spin_lock_init(&mm->pgtable_lock);
    mm->pgd = move(pgd);
    return move(mm);
}

void mm_get(struct mm_struct *mm)
{
    refcount_inc(&mm->urefs);
}

void mm_put(struct mm_struct *mm)
{
    struct rb_node *node, *next;

    might_sleep();
    if (!refcount_dec_and_test(&mm->urefs))
        return;
    rb_for_each_safe (node, next, &mm->vma_tree) {
        struct vm_struct *vma __free(vma) =
            rb_entry(node, struct vm_struct, vm_node);

        vma_remove(vma);
    }
    mm_drop(mm);
}

void mm_grab(struct mm_struct *mm)
{
    refcount_inc(&mm->refs);
}

void mm_drop(struct mm_struct *mm)
{
    if (!refcount_dec_and_test(&mm->refs))
        return;
    Assert(!refcount_read(&mm->urefs), "freeing an mm with active urefs\n");
    pgtable_free(mm->pgd);
    kfree(mm);
}

int mm_clone(struct mm_struct *dst, struct mm_struct *src)
{
    struct rb_node *node;
    int ret = 0;

    mutex_lock(&src->mmap_lock);
    dst->start_brk = src->start_brk;
    dst->brk = src->brk;
    rb_for_each (node, &src->vma_tree) {
        struct vm_struct *old = rb_entry(node, struct vm_struct, vm_node);
        struct vm_struct *vma __free(vma) = vma_alloc();

        if (!vma) {
            ret = -ENOMEM;
            goto out;
        }
        *vma = *old;
        vma->vm_mm = NULL;
        RB_CLEAR_NODE(&vma->vm_node);
        vma->vm_file = fget(old->vm_file);
        ret = vma_insert(dst, vma);
        if (ret)
            goto out;
        keep(vma);
    }
    ret = pgtable_clone(dst, src);
out:
    mutex_unlock(&src->mmap_lock);
    return ret;
}

void switch_mm(struct mm_struct *mm)
{
    struct rq *rq = this_rq();
    struct mm_struct *old __free(mm_put) = NULL;
    struct mm_struct *active __free(mm_drop) = NULL;
    unsigned long flags;

    spin_lock_irqsave(&rq->lock, flags);
    old = current->mm;
    active = rq->active_mm;
    current->mm = mm;
    mm_grab(mm);
    pgtable_switch(mm);
    rq->active_mm = mm;
    spin_unlock_irqrestore(&rq->lock, flags);
}
