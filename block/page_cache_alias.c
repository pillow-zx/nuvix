/* Logical-to-physical association management for the page cache. */

#include <nuvix/errno.h>
#include <nuvix/hashtable.h>
#include <nuvix/slab.h>

#include "internal.h"

/* Zero-initialized hlist buckets are empty. Lookup is expected O(1);
 * owner lists are only used for operations spanning a mapping or page. */
#define PGCACHE_ASSOC_HASH_BITS 10

static HASH_TABLE(pgcache_assoc, PGCACHE_ASSOC_HASH_BITS);

static uint64_t pgcache_assoc_hash(struct page_mapping *mapping, uint64_t index)
{
	uint64_t hash =
		(uint64_t)(uintptr_t)mapping ^ (index * 0x9e3779b97f4a7c15ULL);

	hash ^= hash >> 30;
	hash *= 0xbf58476d1ce4e5b9ULL;
	hash ^= hash >> 27;
	hash *= 0x94d049bb133111ebULL;
	return hash ^ (hash >> 31);
}

struct pgcache_assoc *pgcache_assoc_find_locked(struct page_mapping *mapping,
						uint64_t index)
{
	struct hlist_node *pos;

	hash_table_for_each_possible (pos, &pgcache_assoc,
				      pgcache_assoc_hash(mapping, index)) {
		struct pgcache_assoc *assoc =
			hlist_entry(pos, struct pgcache_assoc, hash_node);

		if (assoc->mapping == mapping && assoc->index == index)
			return assoc;
	}
	return NULL;
}

bool pgcache_assoc_has_page_locked(struct pgcache *page)
{
	return page && !list_empty(&page->associations);
}

int page_cache_assoc_add(struct page_mapping *mapping, uint64_t index,
			 struct pgcache *page)
{
	struct pgcache_assoc *assoc;
	struct pgcache_assoc *existing;
	irq_flags_t flags;

	if (!mapping || !page)
		return -EINVAL;
	assoc = kmalloc(sizeof(*assoc), ALLOC_NOWAIT);
	if (!assoc)
		return -ENOMEM;
	assoc->mapping = mapping;
	assoc->index = index;
	assoc->page = page;
	INIT_HLIST_NODE(&assoc->hash_node);
	INIT_LIST_HEAD(&assoc->page_node);
	INIT_LIST_HEAD(&assoc->mapping_node);

	spin_lock_irqsave(&pgcache_lock, flags);
	existing = pgcache_assoc_find_locked(mapping, index);
	if (existing) {
		int ret = existing->page == page ? 0 : -EAGAIN;

		spin_unlock_irqrestore(&pgcache_lock, flags);
		kfree(assoc);
		return ret;
	}
	hash_table_add(&pgcache_assoc, pgcache_assoc_hash(mapping, index),
		       &assoc->hash_node);
	list_add_tail(&assoc->mapping_node, &mapping->pages);
	list_add_tail(&assoc->page_node, &page->associations);
	spin_unlock_irqrestore(&pgcache_lock, flags);
	return 0;
}

/* Unlink every index before deferring the allocation's free until unlock. */
void pgcache_assoc_remove_locked(struct pgcache_assoc *assoc,
				 struct list_head *removed)
{
	hash_table_del(&assoc->hash_node);
	list_del_init(&assoc->mapping_node);
	list_move_tail(&assoc->page_node, removed);
}

void pgcache_assoc_remove_mapping_locked(struct page_mapping *mapping,
					 struct list_head *removed)
{
	struct list_head *pos, *next;

	if (!mapping)
		return;
	list_for_each_safe (pos, next, &mapping->pages) {
		struct pgcache_assoc *assoc =
			list_entry(pos, struct pgcache_assoc, mapping_node);
		struct pgcache *page = assoc->page;
		pgcache_assoc_remove_locked(assoc, removed);
		if (!pgcache_assoc_has_page_locked(page)) {
			pgcache_clear_dirty_locked(page);
			page->uptodate = false;
		}
	}
}

void pgcache_assoc_remove_page_locked(struct pgcache *page,
				      struct list_head *removed)
{
	struct list_head *pos, *next;

	if (!page || !removed)
		return;
	list_for_each_safe (pos, next, &page->associations) {
		struct pgcache_assoc *assoc =
			list_entry(pos, struct pgcache_assoc, page_node);
		pgcache_assoc_remove_locked(assoc, removed);
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
