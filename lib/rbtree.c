#include <nuvix/rbtree.h>

static bool rb_is_red(const struct rb_node *node)
{
    return node && node->rb_red;
}

static void rb_change_child(struct rb_node *old, struct rb_node *new,
                            struct rb_node *parent, struct rb_root *root)
{
    if (!parent)
        root->rb_node = new;
    else if (parent->rb_left == old)
        parent->rb_left = new;
    else
        parent->rb_right = new;
    if (new)
        new->rb_parent = parent;
}

static void rb_rotate_left(struct rb_node *node, struct rb_root *root)
{
    struct rb_node *next = node->rb_right;

    node->rb_right = next->rb_left;
    if (node->rb_right)
        node->rb_right->rb_parent = node;
    rb_change_child(node, next, node->rb_parent, root);
    next->rb_left = node;
    node->rb_parent = next;
}

static void rb_rotate_right(struct rb_node *node, struct rb_root *root)
{
    struct rb_node *next = node->rb_left;

    node->rb_left = next->rb_right;
    if (node->rb_left)
        node->rb_left->rb_parent = node;
    rb_change_child(node, next, node->rb_parent, root);
    next->rb_right = node;
    node->rb_parent = next;
}

void rb_insert_color(struct rb_node *node, struct rb_root *root)
{
    struct rb_node *parent, *grand, *uncle;

    while ((parent = node->rb_parent) && parent->rb_red) {
        grand = parent->rb_parent;
        if (parent == grand->rb_left) {
            uncle = grand->rb_right;
            if (rb_is_red(uncle)) {
                parent->rb_red = false;
                uncle->rb_red = false;
                grand->rb_red = true;
                node = grand;
                continue;
            }
            if (node == parent->rb_right) {
                node = parent;
                rb_rotate_left(node, root);
                parent = node->rb_parent;
            }
            parent->rb_red = false;
            grand->rb_red = true;
            rb_rotate_right(grand, root);
        } else {
            uncle = grand->rb_left;
            if (rb_is_red(uncle)) {
                parent->rb_red = false;
                uncle->rb_red = false;
                grand->rb_red = true;
                node = grand;
                continue;
            }
            if (node == parent->rb_left) {
                node = parent;
                rb_rotate_right(node, root);
                parent = node->rb_parent;
            }
            parent->rb_red = false;
            grand->rb_red = true;
            rb_rotate_left(grand, root);
        }
    }
    root->rb_node->rb_red = false;
}

static void rb_erase_fixup(struct rb_node *node, struct rb_node *parent,
                           struct rb_root *root)
{
    struct rb_node *sibling;

    while (node != root->rb_node && !rb_is_red(node)) {
        if (node == parent->rb_left) {
            sibling = parent->rb_right;
            if (rb_is_red(sibling)) {
                sibling->rb_red = false;
                parent->rb_red = true;
                rb_rotate_left(parent, root);
                sibling = parent->rb_right;
            }
            if (!rb_is_red(sibling->rb_left) && !rb_is_red(sibling->rb_right)) {
                sibling->rb_red = true;
                node = parent;
                parent = node->rb_parent;
                continue;
            }
            if (!rb_is_red(sibling->rb_right)) {
                sibling->rb_left->rb_red = false;
                sibling->rb_red = true;
                rb_rotate_right(sibling, root);
                sibling = parent->rb_right;
            }
            sibling->rb_red = parent->rb_red;
            parent->rb_red = false;
            sibling->rb_right->rb_red = false;
            rb_rotate_left(parent, root);
        } else {
            sibling = parent->rb_left;
            if (rb_is_red(sibling)) {
                sibling->rb_red = false;
                parent->rb_red = true;
                rb_rotate_right(parent, root);
                sibling = parent->rb_left;
            }
            if (!rb_is_red(sibling->rb_left) && !rb_is_red(sibling->rb_right)) {
                sibling->rb_red = true;
                node = parent;
                parent = node->rb_parent;
                continue;
            }
            if (!rb_is_red(sibling->rb_left)) {
                sibling->rb_right->rb_red = false;
                sibling->rb_red = true;
                rb_rotate_left(sibling, root);
                sibling = parent->rb_left;
            }
            sibling->rb_red = parent->rb_red;
            parent->rb_red = false;
            sibling->rb_left->rb_red = false;
            rb_rotate_right(parent, root);
        }
        node = root->rb_node;
        break;
    }
    if (node)
        node->rb_red = false;
}

void rb_erase(struct rb_node *node, struct rb_root *root)
{
    struct rb_node *successor = node, *child, *parent;
    bool red = node->rb_red;

    if (!node->rb_left) {
        child = node->rb_right;
        parent = node->rb_parent;
        rb_change_child(node, child, parent, root);
    } else if (!node->rb_right) {
        child = node->rb_left;
        parent = node->rb_parent;
        rb_change_child(node, child, parent, root);
    } else {
        successor = node->rb_right;
        while (successor->rb_left)
            successor = successor->rb_left;
        red = successor->rb_red;
        child = successor->rb_right;
        parent = successor->rb_parent;
        if (parent == node) {
            parent = successor;
            if (child)
                child->rb_parent = successor;
        } else {
            rb_change_child(successor, child, parent, root);
            successor->rb_right = node->rb_right;
            successor->rb_right->rb_parent = successor;
        }
        rb_change_child(node, successor, node->rb_parent, root);
        successor->rb_left = node->rb_left;
        successor->rb_left->rb_parent = successor;
        successor->rb_red = node->rb_red;
    }
    if (!red)
        rb_erase_fixup(child, parent, root);
    node->rb_left = node->rb_right = NULL;
    node->rb_red = false;
    RB_CLEAR_NODE(node);
}

struct rb_node *rb_first(const struct rb_root *root)
{
    struct rb_node *node = root->rb_node;

    if (node)
        while (node->rb_left)
            node = node->rb_left;
    return node;
}

struct rb_node *rb_last(const struct rb_root *root)
{
    struct rb_node *node = root->rb_node;

    if (node)
        while (node->rb_right)
            node = node->rb_right;
    return node;
}

struct rb_node *rb_next(const struct rb_node *node)
{
    struct rb_node *parent;

    if (!node || RB_EMPTY_NODE(node))
        return NULL;
    if (node->rb_right) {
        node = node->rb_right;
        while (node->rb_left)
            node = node->rb_left;
        return (struct rb_node *)node;
    }
    while ((parent = node->rb_parent) && node == parent->rb_right)
        node = parent;
    return parent;
}

struct rb_node *rb_prev(const struct rb_node *node)
{
    struct rb_node *parent;

    if (!node || RB_EMPTY_NODE(node))
        return NULL;
    if (node->rb_left) {
        node = node->rb_left;
        while (node->rb_right)
            node = node->rb_right;
        return (struct rb_node *)node;
    }
    while ((parent = node->rb_parent) && node == parent->rb_left)
        node = parent;
    return parent;
}
