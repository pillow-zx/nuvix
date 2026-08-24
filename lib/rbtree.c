/* Intrusive red-black tree balancing. */

#include <nuvix/rbtree.h>

__always_inline __nonnull(1)
static inline void rb_set_red(struct rb_node *node)
{
	node->color = true;
}

__always_inline __nonnull(1)
static inline void rb_set_black(struct rb_node *node)
{
	node->color = false;
}

__nonnull(1, 2)
static void rb_rotate_left(struct rb_node *node, struct rb_root *root)
{
	struct rb_node *right = node->right;

	node->right = right->left;
	if (right->left)
		right->left->parent = node;
	right->parent = node->parent;
	if (!node->parent)
		root->node = right;
	else if (node == node->parent->left)
		node->parent->left = right;
	else
		node->parent->right = right;
	right->left = node;
	node->parent = right;
}

__nonnull(1, 2)
static void rb_rotate_right(struct rb_node *node, struct rb_root *root)
{
	struct rb_node *left = node->left;

	node->left = left->right;
	if (left->right)
		left->right->parent = node;
	left->parent = node->parent;
	if (!node->parent)
		root->node = left;
	else if (node == node->parent->right)
		node->parent->right = left;
	else
		node->parent->left = left;
	left->right = node;
	node->parent = left;
}

void rb_insert_color(struct rb_node *node, struct rb_root *root)
{
	while (node->parent && node->parent->color) {
		struct rb_node *parent = node->parent;
		struct rb_node *grandparent = parent->parent;

		if (parent == grandparent->left) {
			struct rb_node *uncle = grandparent->right;

			if (uncle->color) {
				rb_set_black(parent);
				rb_set_black(uncle);
				rb_set_red(grandparent);
				node = grandparent;
			} else {
				if (node == parent->right) {
					node = parent;
					rb_rotate_left(node, root);
					parent = node->parent;
					grandparent = parent->parent;
				}
				rb_set_black(parent);
				rb_set_red(grandparent);
				rb_rotate_right(grandparent, root);
			}
		} else {
			struct rb_node *uncle = grandparent->left;

			if (uncle->color) {
				rb_set_black(parent);
				rb_set_black(uncle);
				rb_set_red(grandparent);
				node = grandparent;
			} else {
				if (node == parent->left) {
					node = parent;
					rb_rotate_right(node, root);
					parent = node->parent;
					grandparent = parent->parent;
				}
				rb_set_black(parent);
				rb_set_red(grandparent);
				rb_rotate_left(grandparent, root);
			}
		}
	}
	rb_set_black(root->node);
}

static void rb_erase_color(struct rb_node *parent, struct rb_root *root)
{
	struct rb_node *node = NULL;

	while ((!node || !node->color) && node != root->node) {
		struct rb_node *sibling;

		if (!parent)
			break;
		if (parent->left == node) {
			sibling = parent->right;
			if (sibling->color) {
				rb_set_black(sibling);
				rb_set_red(parent);
				rb_rotate_left(parent, root);
				sibling = parent->right;
			}
			if (!sibling) {
				node = parent;
				parent = node->parent;
				continue;
			}
			if ((!sibling->left || !sibling->left->color) &&
			    (!sibling->right || !sibling->right->color)) {
				rb_set_red(sibling);
				node = parent;
				parent = node->parent;
			} else {
				if (!sibling->right || !sibling->right->color) {
					rb_set_black(sibling->left);
					rb_set_red(sibling);
					rb_rotate_right(sibling, root);
					sibling = parent->right;
				}
				sibling->color = parent->color;
				rb_set_black(parent);
				rb_set_black(sibling->right);
				rb_rotate_left(parent, root);
				node = root->node;
				break;
			}
		} else {
			sibling = parent->left;
			if (sibling->color) {
				rb_set_black(sibling);
				rb_set_red(parent);
				rb_rotate_right(parent, root);
				sibling = parent->left;
			}
			if (!sibling) {
				node = parent;
				parent = node->parent;
				continue;
			}
			if ((!sibling->left || !sibling->left->color) &&
			    (!sibling->right || !sibling->right->color)) {
				rb_set_red(sibling);
				node = parent;
				parent = node->parent;
			} else {
				if (!sibling->left || !sibling->left->color) {
					rb_set_black(sibling->right);
					rb_set_red(sibling);
					rb_rotate_left(sibling, root);
					sibling = parent->left;
				}
				sibling->color = parent->color;
				rb_set_black(parent);
				rb_set_black(sibling->left);
				rb_rotate_right(parent, root);
				node = root->node;
				break;
			}
		}
	}
	rb_set_black(node);
}

void rb_erase(struct rb_node *node, struct rb_root *root)
{
	struct rb_node *child;
	struct rb_node *parent;
	bool red;

	if (!node->left)
		child = node->right;
	else if (!node->right)
		child = node->left;
	else {
		struct rb_node *successor = node->right;

		while (successor->left)
			successor = successor->left;
		child = successor->right;
		parent = successor->parent;
		red = successor->color;
		if (successor != node->right) {
			if (child)
				child->parent = parent;
			parent->left = child;
			successor->right = node->right;
			node->right->parent = successor;
		} else {
			parent = successor;
		}
		successor->left = node->left;
		node->left->parent = successor;
		successor->parent = node->parent;
		successor->color = node->color;
		if (!node->parent)
			root->node = successor;
		else if (node == node->parent->left)
			node->parent->left = successor;
		else
			node->parent->right = successor;
		if (!red)
			rb_erase_color(parent, root);
		return;
	}

	parent = node->parent;
	red = node->color;
	if (child)
		child->parent = parent;
	if (!parent)
		root->node = child;
	else if (node == parent->left)
		parent->left = child;
	else
		parent->right = child;
	if (!red)
		rb_erase_color(parent, root);
}
