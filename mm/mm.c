#include <nuvix/mm.h>
#include <nuvix/pgtable.h>
#include <nuvix/slab.h>
#include <nuvix/assert.h>

struct mm_struct *mm_alloc(void)
{
    struct mm_struct *mm __free(kfree) = kzalloc(sizeof(*mm));
    if (!mm)
        return NULL;

    paddr_t pgd __free(pgd) = 0;
    int ret = alloc_pgd(&pgd);
    if (ret)
        return NULL;
    refcount_set(&mm->refcount, 1);
    mutex_init(&mm->mmap_lock);
    mm->vma_tree = RB_ROOT;
    spin_lock_init(&mm->pgtable_lock);
    mm->pgd = move(pgd);
    return move(mm);
}

void mm_get(struct mm_struct *mm)
{
    refcount_inc(&mm->refcount);
}

void mm_put(struct mm_struct *mm)
{
    if (!refcount_dec_and_test(&mm->refcount))
        return;
    Assert(RB_EMPTY_ROOT(&mm->vma_tree),
           "mm destruction requires an empty VMA tree\n");
    free_pgd(mm->pgd);
    kfree(mm);
}
