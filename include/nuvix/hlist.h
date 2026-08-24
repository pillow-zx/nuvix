#ifndef _NUVIX_HLIST_H
#define _NUVIX_HLIST_H

/**
 * @file hlist.h
 * @brief Intrusive singly linked lists with back-pointers.
 */

#include <nuvix/compiler.h>
#include <nuvix/tools.h>
#include <nuvix/types.h>

struct hlist_node {
	struct hlist_node *next;
	struct hlist_node **pprev;
};

struct hlist_head {
	struct hlist_node *first;
};

#define HLIST_HEAD_INIT	 {.first = NULL}
#define HLIST_HEAD(name) struct hlist_head name = HLIST_HEAD_INIT

__always_inline __nonnull(1)
static inline void INIT_HLIST_HEAD(struct hlist_head *head)
{
	head->first = NULL;
}

__always_inline __nonnull(1)
static inline void INIT_HLIST_NODE(struct hlist_node *node)
{
	node->next = NULL;
	node->pprev = NULL;
}

__always_inline __must_check __pure
static inline bool hlist_empty(const struct hlist_head *head)
{
	return head->first == NULL;
}

__always_inline __must_check __pure
static inline bool hlist_unhashed(const struct hlist_node *node)
{
	return node->pprev == NULL;
}

__nonnull(1)
static inline void hlist_add_head(struct hlist_node *node, struct hlist_head *head)
{
	struct hlist_node *first = head->first;

	node->next = first;
	node->pprev = &head->first;
	if (first)
		first->pprev = &node->next;
	head->first = node;
}

__nonnull(1)
static inline void hlist_del(struct hlist_node *node)
{
	if (!node->pprev)
		return;

	if (node->next)
		node->next->pprev = node->pprev;
	*node->pprev = node->next;
	node->next = NULL;
	node->pprev = NULL;
}

__always_inline __nonnull(1)
static inline void hlist_del_init(struct hlist_node *node)
{
	hlist_del(node);
}

#define hlist_entry(ptr, type, member) container_of(ptr, type, member)

#define hlist_for_each(pos, head)                                              \
	for ((pos) = (head)->first; (pos) != NULL; (pos) = (pos)->next)

#define hlist_for_each_safe(pos, next, head)                                   \
	for ((pos) = (head)->first, (next) = (pos) ? (pos)->next : NULL;       \
	     (pos) != NULL;                                                    \
	     (pos) = (next), (next) = (pos) ? (pos)->next : NULL)

#define hlist_for_each_entry(pos, head, member)                                \
	for ((pos) = (head)->first ? hlist_entry((head)->first,                \
						 typeof(*(pos)), member)       \
				   : NULL;                                     \
	     (pos) != NULL;                                                    \
	     (pos) = (pos)->member.next ? hlist_entry((pos)->member.next,      \
						      typeof(*(pos)), member)  \
					: NULL)

#endif
