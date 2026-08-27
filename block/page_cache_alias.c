/* Logical-to-physical association management for the page cache. */

#include <nuvix/errno.h>
#include <nuvix/slab.h>

#include "internal.h"

bool pgcache_assoc_has_page_locked(struct pgcache *page)
{
	struct list_head *pos;

	if (!page)
		return false;
	list_for_each (pos, &pgcache_associations) {
		struct pgcache_assoc *assoc =
			list_entry(pos, struct pgcache_assoc, mapping_node);

		if (assoc->page == page)
			return true;
	}
	return false;
}

int page_cache_assoc_add(struct page_mapping *mapping, uint64_t index,
			 struct pgcache *page)
{
	struct pgcache_assoc *assoc;
	struct list_head *pos;
	irq_flags_t flags;

	if (!mapping || !page)
		return -EINVAL;
	assoc = kmalloc(sizeof(*assoc), ALLOC_NOWAIT);
	if (!assoc)
		return -ENOMEM;
	assoc->mapping = mapping;
	assoc->index = index;
	assoc->page = page;
	INIT_LIST_HEAD(&assoc->page_node);
	INIT_LIST_HEAD(&assoc->mapping_node);

	spin_lock_irqsave(&pgcache_lock, &flags);
	list_for_each (pos, &pgcache_associations) {
		struct pgcache_assoc *existing =
			list_entry(pos, struct pgcache_assoc, mapping_node);

		if (existing->mapping == mapping && existing->index == index) {
			spin_unlock_irqrestore(&pgcache_lock, flags);
			kfree(assoc);
			return 0;
		}
	}
	list_add_tail(&assoc->mapping_node, &pgcache_associations);
	spin_unlock_irqrestore(&pgcache_lock, flags);
	return 0;
}

void pgcache_assoc_remove_mapping(struct page_mapping *mapping)
{
	struct list_head *pos, *next;
	LIST_HEAD(removed);
	irq_flags_t flags;

	if (!mapping)
		return;
	spin_lock_irqsave(&pgcache_lock, &flags);
	list_for_each_safe (pos, next, &pgcache_associations) {
		struct pgcache_assoc *assoc =
			list_entry(pos, struct pgcache_assoc, mapping_node);
		if (assoc->mapping != mapping)
			continue;
		struct pgcache *page = assoc->page;
		list_del_init(&assoc->mapping_node);
		list_add_tail(&assoc->page_node, &removed);
		if (!pgcache_assoc_has_page_locked(page)) {
			pgcache_clear_dirty_locked(page);
			page->uptodate = false;
		}
	}
	spin_unlock_irqrestore(&pgcache_lock, flags);
	pgcache_assoc_free_list(&removed);
}

void pgcache_assoc_remove_page_locked(struct pgcache *page,
				      struct list_head *removed)
{
	struct list_head *pos, *next;

	if (!page || !removed)
		return;
	list_for_each_safe (pos, next, &pgcache_associations) {
		struct pgcache_assoc *assoc =
			list_entry(pos, struct pgcache_assoc, mapping_node);
		if (assoc->page != page)
			continue;
		list_del_init(&assoc->mapping_node);
		list_add_tail(&assoc->page_node, removed);
	}
}

void pgcache_assoc_free_list(struct list_head *removed)
{
	struct list_head *pos, *next;

	if (!removed)
		return;
	list_for_each_safe (pos, next, removed) {
		struct pgcache_assoc *assoc =
			list_entry(pos, struct pgcache_assoc, page_node);

		list_del_init(&assoc->page_node);
		kfree(assoc);
	}
}
