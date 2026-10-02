#ifndef _NUVIX_RBTREE_H
#define _NUVIX_RBTREE_H

#include <nuvix/ctypes.h>
#include <nuvix/compiler.h>
#include <nuvix/macro.h>

struct rb_node {
    struct rb_node *rb_parent;
    struct rb_node *rb_left;
    struct rb_node *rb_right;
    bool rb_red;
};

struct rb_root {
    struct rb_node *rb_node;
};

#define RB_ROOT                     ((struct rb_root){NULL})
#define rb_parent(node)             ((node)->rb_parent)
#define rb_entry(ptr, type, member) container_of(ptr, type, member)
#define RB_EMPTY_ROOT(root)         ((root)->rb_node == NULL)
#define RB_EMPTY_NODE(node)         ((node)->rb_parent == (node))
#define RB_CLEAR_NODE(node)         ((node)->rb_parent = (node))

static inline void rb_link_node(struct rb_node *node, struct rb_node *parent,
                                struct rb_node **link)
{
    node->rb_parent = parent;
    node->rb_left = node->rb_right = NULL;
    node->rb_red = true;
    *link = node;
}

void rb_insert_color(struct rb_node *node, struct rb_root *root);

void rb_erase(struct rb_node *node, struct rb_root *root);
struct rb_node *rb_first(const struct rb_root *root);
struct rb_node *rb_last(const struct rb_root *root);
struct rb_node *rb_next(const struct rb_node *node);
struct rb_node *rb_prev(const struct rb_node *node);

#define rb_for_each(pos, root)                                                 \
    for ((pos) = rb_first(root); (pos); (pos) = rb_next(pos))

#define rb_for_each_safe(pos, next, root)                                      \
    for ((pos) = rb_first(root); (pos) && (((next) = rb_next(pos)), 1);        \
         (pos) = (next))

#define rb_entry_safe(ptr, type, member)                                       \
    ({                                                                         \
        TYPEOF(ptr) __node = (ptr);                                            \
        __node ? rb_entry(__node, type, member) : NULL;                        \
    })

#define rb_for_each_entry(pos, root, member)                                   \
    for ((pos) = rb_entry_safe(rb_first(root), TYPEOF(*(pos)), member); (pos); \
         (pos) =                                                               \
             rb_entry_safe(rb_next(&(pos)->member), TYPEOF(*(pos)), member))

#define rb_for_each_entry_safe(pos, next, root, member)                        \
    for ((pos) = rb_entry_safe(rb_first(root), TYPEOF(*(pos)), member);        \
         (pos) && (((next) = rb_entry_safe(rb_next(&(pos)->member),            \
                                           TYPEOF(*(pos)), member)),           \
                   1);                                                         \
         (pos) = (next))

#endif /* _NUVIX_RBTREE_H */
