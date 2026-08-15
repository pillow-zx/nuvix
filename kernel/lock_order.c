/* Debug-only equal-rank lock order enforcement. */

#ifdef CONFIG_DEBUG_CONTEXT

#include <nuvix/spinlock.h>
#include <nuvix/tools.h>

extern spinlock_t wb_buf_lock;
extern spinlock_t vblk_submit_lock;

struct lock_order_pair {
	const spinlock_t *first;
	const spinlock_t *second;
};

/* Equal-rank pairs that are allowed to nest, in their fixed acquisition
 * order.  Reverse acquisition of a registered pair is a debug panic. */
static const struct lock_order_pair equal_rank_orders[] = {
	{ &wb_buf_lock, &vblk_submit_lock },
};

bool lock_equal_rank_order_ok(const spinlock_t *prev, const spinlock_t *next)
{
	for (size_t i = 0; i < ARRLEN(equal_rank_orders); i++) {
		if (prev == equal_rank_orders[i].first &&
		    next == equal_rank_orders[i].second)
			return true;
		if (prev == equal_rank_orders[i].second &&
		    next == equal_rank_orders[i].first)
			return false;
	}

	return true;
}

#endif
