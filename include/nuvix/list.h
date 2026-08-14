#ifndef _NUVIX_LIST_H
#define _NUVIX_LIST_H

/**
 * @file list.h
 * @brief Intrusive circular doubly linked list helpers.
 */

#include <nuvix/types.h>
#include <nuvix/tools.h>

/**
 * @struct list_head
 * @brief Link node embedded into objects participating in intrusive lists.
 *
 * @par Fields
 * - @c prev: Previous node, or head for first element.
 * - @c next: Next node, or head for last element.
 */
struct list_head {
	struct list_head *prev;
	struct list_head *next;
};

/**
 * @def LIST_HEAD_INIT
 * @brief Static initializer for an empty circular list head.
 * @param name List head variable being initialized.
 */
#define LIST_HEAD_INIT(name) {&(name), &(name)}

/**
 * @def LIST_HEAD
 * @brief Define and initialize a list head variable.
 * @param name Variable name.
 */
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

/**
 * @def LIST_HEAD_STATIC
 * @brief Static define and initialize a list head variable.
 * @param name Variable name.
 */
#define LIST_HEAD_STATIC(name) static struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list)
{
	list->prev = list;
	list->next = list;
}

static inline void __list_add(struct list_head *new_node,
				       struct list_head *prev,
				       struct list_head *next)
{
	next->prev = new_node;
	new_node->next = next;
	new_node->prev = prev;
	prev->next = new_node;
}

static inline void list_add(struct list_head *new_node,
				     struct list_head *head)
{
	__list_add(new_node, head, head->next);
}

static inline void list_add_tail(struct list_head *new_node,
					  struct list_head *head)
{
	__list_add(new_node, head->prev, head);
}

static inline void __list_del(struct list_head *prev,
				       struct list_head *next)
{
	next->prev = prev;
	prev->next = next;
}

static inline void list_del(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
	entry->prev = NULL;
	entry->next = NULL;
}

static inline void list_del_init(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
	INIT_LIST_HEAD(entry);
}

static inline void list_move(struct list_head *list,
				      struct list_head *head)
{
	__list_del(list->prev, list->next);
	list_add(list, head);
}

static inline void list_move_tail(struct list_head *list,
					   struct list_head *head)
{
	__list_del(list->prev, list->next);
	list_add_tail(list, head);
}

__must_check __pure
static inline bool list_empty(const struct list_head *head)
{
	return head->next == head;
}

/**
 * @def list_for_each
 * @brief Iterate raw list_head nodes in forward order.
 * @param pos Cursor of type `struct list_head *`.
 * @param head List head.
 */
#define list_for_each(pos, head)                                               \
	for (pos = (head)->next; pos != (head); pos = pos->next)

/**
 * @def list_for_each_safe
 * @brief Iterate raw nodes while allowing deletion of the current node.
 * @param pos Current node cursor.
 * @param n Temporary cursor storing the next node.
 * @param head List head.
 */
#define list_for_each_safe(pos, n, head)                                       \
	for (pos = (head)->next, n = pos->next; pos != (head);                 \
	     pos = n, n = pos->next)

/**
 * @def list_entry
 * @brief Recover the containing object from an embedded list node.
 * @param ptr Pointer to the embedded list_head.
 * @param type Containing object type.
 * @param member Name of the list_head member inside @p type.
 */
#define list_entry(ptr, type, member) container_of(ptr, type, member)

/**
 * @def list_first_entry
 * @brief Return the first object in a non-empty intrusive list.
 */
#define list_first_entry(ptr, type, member)                                    \
	list_entry((ptr)->next, type, member)

/**
 * @def list_for_each_entry
 * @brief Iterate containing objects in forward order.
 * @param pos Cursor pointer to the containing type.
 * @param head List head.
 * @param member Embedded list_head member name.
 *
 * The macro infers the containing type from `*pos`, then converts each list
 * node back to its owner with @ref list_entry.
 */
#define list_for_each_entry(pos, head, member)                                 \
	for (pos = list_entry((head)->next, typeof(*pos), member);             \
	     &pos->member != (head);                                           \
	     pos = list_entry(pos->member.next, typeof(*pos), member))

#endif
