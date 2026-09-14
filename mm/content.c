/* Logical private and shared-anonymous content; PTEs hold separate refs. */
#include <nuvix/buddy.h>
#include <nuvix/errno.h>
#include <nuvix/slab.h>
#include "internal.h"

static bool anon_covered(struct anon_shared *anon, uintptr_t index);

static struct mm_page_slot *slot_find(struct rb_root *root, uintptr_t index)
{
	struct rb_node *node = root->node;

	while (node) {
		struct mm_page_slot *slot =
			rb_entry(node, struct mm_page_slot, node);
		if (index == slot->index)
			return slot;
		node = index < slot->index ? node->left : node->right;
	}
	return NULL;
}

static void slot_insert(struct rb_root *root, struct mm_page_slot *slot)
{
	struct rb_node **link = &root->node, *parent = NULL;

	while (*link) {
		struct mm_page_slot *other =
			rb_entry(*link, struct mm_page_slot, node);
		parent = *link;
		BUG_ON(slot->index == other->index);
		link = slot->index < other->index ? &parent->left
						  : &parent->right;
	}
	rb_link_node(&slot->node, parent, link);
	rb_insert_color(&slot->node, root);
}

struct mm_page_slot *mm_private_find(struct mm_struct *mm, uintptr_t va)
{
	return slot_find(&mm->private, va >> PAGE_SHIFT);
}

/* Caller holds mmap_lock. Success takes an additional slot reference. */
int mm_private_set(struct mm_struct *mm, uintptr_t va, struct page *page,
		   bool cow)
{
	struct mm_page_slot *slot = mm_private_find(mm, va);

	if (!slot) {
		slot = kmalloc(sizeof(*slot), ALLOC_NOWAIT);
		if (!slot)
			return -ENOMEM;
		slot->index = va >> PAGE_SHIFT;
		slot->page = NULL;
		slot_insert(&mm->private, slot);
	}
	page_get(page);
	if (slot->page)
		page_put(slot->page);
	slot->page = page;
	slot->cow = cow;
	return 0;
}

void mm_private_remove(struct mm_struct *mm, uintptr_t start, uintptr_t end)
{
	struct rb_node *node, *next;

	rb_for_each_safe (node, next, &mm->private) {
		struct mm_page_slot *slot =
			rb_entry(node, struct mm_page_slot, node);
		uintptr_t va = slot->index << PAGE_SHIFT;
		if (va < start || va >= end)
			continue;
		rb_erase(node, &mm->private);
		page_put(slot->page);
		kfree(slot);
	}
}

/* Both address spaces are exclusively held; child remains unpublished. */
int mm_private_clone(struct mm_struct *child, struct mm_struct *parent)
{
	struct rb_node *node;

	rb_for_each (node, &parent->private) {
		struct mm_page_slot *slot =
			rb_entry(node, struct mm_page_slot, node);
		int ret = mm_private_set(child, slot->index << PAGE_SHIFT,
					 slot->page, true);
		if (ret < 0)
			return ret;
	}
	rb_for_each (node, &parent->private) {
		struct mm_page_slot *slot =
			rb_entry(node, struct mm_page_slot, node);
		slot->cow = true;
	}
	return 0;
}

struct anon_shared *mm_anon_create(void)
{
	struct anon_shared *anon = kmalloc(sizeof(*anon), ALLOC_NOWAIT);

	if (!anon)
		return NULL;
	refcount_set(&anon->refs, 1);
	anon->pages = RB_ROOT;
	INIT_LIST_HEAD(&anon->mappings);
	spin_lock_init(&anon->lock, LOCK_RANK_MM_ANON, LOCK_IRQ_TASK_ONLY);
	return anon;
}

void mm_anon_get(struct anon_shared *anon)
{
	if (anon)
		refcount_inc(&anon->refs);
}

void mm_anon_put(struct anon_shared *anon)
{
	struct rb_node *node, *next;

	if (!anon || !refcount_dec_and_test(&anon->refs))
		return;
	rb_for_each_safe (node, next, &anon->pages) {
		struct mm_page_slot *slot =
			rb_entry(node, struct mm_page_slot, node);
		rb_erase(node, &anon->pages);
		page_put(slot->page);
		kfree(slot);
	}
	kfree(anon);
}

/* Return a held page. Allocation never happens under the object spinlock. */
struct page *mm_anon_page(struct anon_shared *anon, uintptr_t index)
{
	struct mm_page_slot *slot, *prepared;
	struct page *page;
	void *data;

	spin_lock(&anon->lock);
	if (!anon_covered(anon, index)) {
		spin_unlock(&anon->lock);
		return NULL;
	}
	slot = slot_find(&anon->pages, index);
	if (slot) {
		page = slot->page;
		page_get(page);
		spin_unlock(&anon->lock);
		return page;
	}
	spin_unlock(&anon->lock);
	prepared = kmalloc(sizeof(*prepared), ALLOC_NOWAIT);
	if (!prepared)
		return NULL;
	data = get_free_page(0, ALLOC_NOWAIT);
	if (!data) {
		kfree(prepared);
		return NULL;
	}
	memset(data, 0, PAGE_SIZE);
	prepared->index = index;
	prepared->page = virt_to_page(data);
	prepared->cow = false;
	spin_lock(&anon->lock);
	if (!anon_covered(anon, index)) {
		spin_unlock(&anon->lock);
		page_put(prepared->page);
		kfree(prepared);
		return NULL;
	}
	slot = slot_find(&anon->pages, index);
	if (!slot) {
		slot_insert(&anon->pages, prepared);
		slot = prepared;
		prepared = NULL;
	}
	page = slot->page;
	page_get(page);
	spin_unlock(&anon->lock);
	if (prepared) {
		page_put(prepared->page);
		kfree(prepared);
	}
	return page;
}

/* The permanent owner holds one reference; callers receive a separate ref. */
static struct page *zero_page;
static DEFINE_SPINLOCK(zero_lock, LOCK_RANK_MM_ANON, LOCK_IRQ_TASK_ONLY);

struct page *mm_zero_page(void)
{
	struct page *page, *prepared;
	void *data;

	spin_lock(&zero_lock);
	page = zero_page;
	if (page)
		page_get(page);
	spin_unlock(&zero_lock);
	if (page)
		return page;
	data = get_free_page(0, ALLOC_NOWAIT);
	if (!data)
		return NULL;
	memset(data, 0, PAGE_SIZE);
	prepared = virt_to_page(data);
	spin_lock(&zero_lock);
	if (!zero_page) {
		zero_page = prepared;
		prepared = NULL;
	}
	page = zero_page;
	page_get(page);
	spin_unlock(&zero_lock);
	if (prepared)
		page_put(prepared);
	return page;
}

static bool anon_covered(struct anon_shared *anon, uintptr_t index)
{
	struct list_head *node;

	list_for_each (node, &anon->mappings) {
		struct vm_area_struct *vma =
			list_entry(node, struct vm_area_struct, anon_link);

		if (index >= vma->anon_first && index < vma->anon_end)
			return true;
	}
	return false;
}

/* Object lock held; releases use only the non-sleeping allocators. */
static void anon_prune(struct anon_shared *anon)
{
	struct rb_node *node, *next;

	rb_for_each_safe (node, next, &anon->pages) {
		struct mm_page_slot *slot =
			rb_entry(node, struct mm_page_slot, node);

		if (anon_covered(anon, slot->index))
			continue;
		rb_erase(node, &anon->pages);
		page_put(slot->page);
		kfree(slot);
	}
}

void mm_anon_register(struct vm_area_struct *vma)
{
	struct anon_shared *anon = vma->vm_anon;

	if (!anon || vma->anon_registered)
		return;
	spin_lock(&anon->lock);
	vma->anon_first = vma->vm_offset >> PAGE_SHIFT;
	vma->anon_end =
		vma->anon_first + ((vma->vm_end - vma->vm_start) >> PAGE_SHIFT);
	list_add_tail(&vma->anon_link, &anon->mappings);
	vma->anon_registered = true;
	spin_unlock(&anon->lock);
}

void mm_anon_update(struct vm_area_struct *vma)
{
	struct anon_shared *anon = vma->vm_anon;

	if (!anon || !vma->anon_registered)
		return;
	spin_lock(&anon->lock);
	vma->anon_first = vma->vm_offset >> PAGE_SHIFT;
	vma->anon_end =
		vma->anon_first + ((vma->vm_end - vma->vm_start) >> PAGE_SHIFT);
	anon_prune(anon);
	spin_unlock(&anon->lock);
}

void mm_anon_unregister(struct vm_area_struct *vma)
{
	struct anon_shared *anon = vma->vm_anon;

	if (!anon || !vma->anon_registered)
		return;
	spin_lock(&anon->lock);
	list_del(&vma->anon_link);
	vma->anon_registered = false;
	anon_prune(anon);
	spin_unlock(&anon->lock);
}
