/* Physical page-cache dirty-list management. */

#include "internal.h"

#include <nuvix/errno.h>

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
	page->error = 0;
	page->dirty_generation++;
	spin_unlock_irqrestore(&pgcache_lock, flags);
}

int pgcache_shared_write_begin(struct pgcache *page)
{
	irq_flags_t flags;

	if (!page)
		return -EINVAL;
	spin_lock_irqsave(&pgcache_lock, &flags);
	if (page->dropped || page->invalidating) {
		spin_unlock_irqrestore(&pgcache_lock, flags);
		return -EBUSY;
	}
	page->writable_pte_count++;
	if (!page->dirty)
		list_add_tail(&page->dirty_node, &pgcache_dirty_list);
	page->dirty = true;
	page->error = 0;
	page->dirty_generation++;
	spin_unlock_irqrestore(&pgcache_lock, flags);
	return 0;
}

void pgcache_shared_write_end(struct pgcache *page)
{
	irq_flags_t flags;

	if (!page)
		return;
	spin_lock_irqsave(&pgcache_lock, &flags);
	BUG_ON(page->writable_pte_count == 0);
	page->writable_pte_count--;
	spin_unlock_irqrestore(&pgcache_lock, flags);
	wait_channel_wake_all(&page->waitq);
}

bool pgcache_has_shared_writers(const struct pgcache *page)
{
	irq_flags_t flags;
	bool result;

	if (!page)
		return false;
	spin_lock_irqsave(&pgcache_lock, &flags);
	result = page->writable_pte_count != 0;
	spin_unlock_irqrestore(&pgcache_lock, flags);
	return result;
}

int pgcache_mutation_begin(struct pgcache *page)
{
	irq_flags_t flags;
	bool invalidating;

	if (!page)
		return -EINVAL;
	spin_lock_irqsave(&pgcache_lock, &flags);
	invalidating = page->invalidating || page->dropped;
	spin_unlock_irqrestore(&pgcache_lock, flags);
	return invalidating ? -EBUSY : 0;
}

void pgcache_mutation_end(struct pgcache *page, bool dirty)
{
	if (dirty)
		pgcache_mark_dirty(page);
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
