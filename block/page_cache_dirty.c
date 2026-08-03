/* Physical page-cache dirty-list management. */

#include "internal.h"

void page_cache_clear_dirty_locked(struct page_cache *page)
{
	if (!page)
		return;
	if (!list_empty(&page->dirty_node))
		list_del_init(&page->dirty_node);
	page->dirty = false;
}

void page_cache_clear_dirty(struct page_cache *page)
{
	irq_flags_t flags;
	if (!page)
		return;
	spin_lock_irqsave(&pgcache_lock, &flags);
	page_cache_clear_dirty_locked(page);
	spin_unlock_irqrestore(&pgcache_lock, flags);
}

void page_cache_mark_dirty(struct page_cache *page)
{
	irq_flags_t flags;
	if (!page)
		return;
	spin_lock_irqsave(&pgcache_lock, &flags);
	if (!page->dirty)
		list_add_tail(&page->dirty_node, &pgcache_dirty_list);
	page->dirty = true;
	page->uptodate = true;
	spin_unlock_irqrestore(&pgcache_lock, flags);
}

struct page_cache *page_cache_dirty_any(void)
{
	struct page_cache *page;
	irq_flags_t flags;

	spin_lock_irqsave(&pgcache_lock, &flags);
	page = page_cache_dirty_any_locked();
	if (page)
		page->refcount++;
	spin_unlock_irqrestore(&pgcache_lock, flags);
	return page;
}

struct page_cache *page_cache_dirty_any_locked(void)
{
	if (list_empty(&pgcache_dirty_list))
		return NULL;
	return list_first_entry(&pgcache_dirty_list, struct page_cache,
				dirty_node);
}
