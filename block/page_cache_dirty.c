/* Physical page-cache dirty-list management. */

#include "internal.h"

void pgcache_clear_dirty_locked(struct pgcache *page)
{
	if (!page)
		return;
	if (!list_empty(&page->dirty_node))
		list_del_init(&page->dirty_node);
	page->dirty = false;
}

void pgcache_clear_dirty(struct pgcache *page)
{
	irq_flags_t flags;
	if (!page)
		return;
	spin_lock_irqsave(&pgcache_lock, &flags);
	pgcache_clear_dirty_locked(page);
	spin_unlock_irqrestore(&pgcache_lock, flags);
}

void pgcache_mark_dirty(struct pgcache *page)
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

struct pgcache *pgcache_dirty_any(void)
{
	struct pgcache *page;
	irq_flags_t flags;

	spin_lock_irqsave(&pgcache_lock, &flags);
	page = pgcache_dirty_any_locked();
	if (page)
		page->refcount++;
	spin_unlock_irqrestore(&pgcache_lock, flags);
	return page;
}

struct pgcache *pgcache_dirty_any_locked(void)
{
	if (list_empty(&pgcache_dirty_list))
		return NULL;
	return list_first_entry(&pgcache_dirty_list, struct pgcache,
				dirty_node);
}
