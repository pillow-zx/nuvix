#ifndef _NUVIX_RBTREE_H
#define _NUVIX_RBTREE_H

/**
 * @file rbtree.h
 * @brief Intrusive red-black tree primitives.
 *
 * Nodes are owned by their containing objects. The tree does not allocate,
 * compare, or free nodes; callers link a node at the position selected by
 * their key ordering and then call rb_insert_color().
 */

#include <nuvix/compiler.h>
#include <nuvix/tools.h>
#include <nuvix/types.h>

struct rb_node {
	struct rb_node *left;
	struct rb_node *right;
	struct rb_node *parent;
	bool color;
};

struct rb_root {
	struct rb_node *node;
};

#define RB_ROOT		    ((struct rb_root){.node = NULL})
#define RB_EMPTY_ROOT(root) ((root)->node == NULL)

__always_inline __nonnull(1, 3)
static inline void rb_link_node(struct rb_node *node, struct rb_node *parent,
                                struct rb_node **link)
{
	node->left = NULL;
	node->right = NULL;
	node->parent = parent;
	node->color = true;
	*link = node;
}

__must_check __pure
static inline struct rb_node *rb_first(const struct rb_root *root)
{
	struct rb_node *node = root->node;

	if (!node)
		return NULL;
	while (node->left)
		node = node->left;
	return node;
}

__must_check __pure
static inline struct rb_node *rb_last(const struct rb_root *root)
{
	struct rb_node *node = root->node;

	if (!node)
		return NULL;
	while (node->right)
		node = node->right;
	return node;
}

__must_check
static inline struct rb_node *rb_next(struct rb_node *node)
{
	struct rb_node *parent;

	if (node->right) {
		node = node->right;
		while (node->left)
			node = node->left;
		return node;
	}

	parent = node->parent;
	while (parent && node == parent->right) {
		node = parent;
		parent = parent->parent;
	}
	return parent;
}

__must_check
static inline struct rb_node *rb_prev(struct rb_node *node)
{
	struct rb_node *parent;

	if (node->left) {
		node = node->left;
		while (node->right)
			node = node->right;
		return node;
	}

	parent = node->parent;
	while (parent && node == parent->left) {
		node = parent;
		parent = parent->parent;
	}
	return parent;
}

#define rb_entry(ptr, type, member) container_of(ptr, type, member)

#define rb_for_each(pos, root)                                                 \
	for ((pos) = rb_first(root); (pos) != NULL; (pos) = rb_next(pos))

#define rb_for_each_safe(pos, next, root)                                      \
	for ((pos) = rb_first(root), (next) = (pos) ? rb_next(pos) : NULL;     \
	     (pos) != NULL;                                                    \
	     (pos) = (next), (next) = (pos) ? rb_next(pos) : NULL)

#define rb_for_each_entry(pos, root, member)                                   \
	for ((pos) = (root)->node ? rb_entry(rb_first(root), typeof(*(pos)),   \
					     member)                           \
				  : NULL;                                      \
	     (pos) != NULL;                                                    \
	     (pos) = rb_next(&(pos)->member)                                   \
			     ? rb_entry(rb_next(&(pos)->member),               \
					typeof(*(pos)), member)                \
			     : NULL)

void rb_insert_color(struct rb_node *node, struct rb_root *root);
void rb_erase(struct rb_node *node, struct rb_root *root);

#endif
