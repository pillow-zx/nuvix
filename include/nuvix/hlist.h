#ifndef _NUVIX_HLIST_H
#define _NUVIX_HLIST_H

#include <nuvix/ctypes.h>
#include <nuvix/compiler.h>
#include <nuvix/macro.h>

struct hlist_node {
    struct hlist_node *next;
    struct hlist_node **pprev;
};

struct hlist_head {
    struct hlist_node *first;
};

#define HLIST_HEAD_INIT  {NULL}
#define HLIST_HEAD(name) struct hlist_head name = HLIST_HEAD_INIT

static inline void INIT_HLIST_HEAD(struct hlist_head *head)
{
    head->first = NULL;
}

static inline void INIT_HLIST_NODE(struct hlist_node *node)
{
    node->next = NULL;
    node->pprev = NULL;
}

static inline bool hlist_empty(const struct hlist_head *head)
{
    return !head->first;
}

static inline bool hlist_unhashed(const struct hlist_node *node)
{
    return !node->pprev;
}

static inline void hlist_add_head(struct hlist_node *node,
                                  struct hlist_head *head)
{
    struct hlist_node *first = head->first;

    node->next = first;
    node->pprev = &head->first;
    if (first)
        first->pprev = &node->next;
    head->first = node;
}

static inline void hlist_del(struct hlist_node *node)
{
    struct hlist_node *next = node->next;

    *node->pprev = next;
    if (next)
        next->pprev = node->pprev;
    INIT_HLIST_NODE(node);
}

static inline void hlist_del_init(struct hlist_node *node)
{
    if (!hlist_unhashed(node))
        hlist_del(node);
}

#define hlist_entry(ptr, type, member) container_of(ptr, type, member)

#define hlist_entry_safe(ptr, type, member)                                    \
    ({                                                                         \
        TYPEOF(ptr) __node = (ptr);                                            \
        __node ? hlist_entry(__node, type, member) : NULL;                     \
    })

#define hlist_for_each(pos, head)                                              \
    for ((pos) = (head)->first; (pos); (pos) = (pos)->next)

#define hlist_for_each_safe(pos, n, head)                                      \
    for ((pos) = (head)->first; (pos) && (((n) = (pos)->next), 1); (pos) = (n))

#define hlist_for_each_entry(pos, head, member)                                \
    for ((pos) = hlist_entry_safe((head)->first, TYPEOF(*(pos)), member);      \
         (pos);                                                                \
         (pos) = hlist_entry_safe((pos)->member.next, TYPEOF(*(pos)), member))

#define hlist_for_each_entry_safe(pos, n, head, member)                        \
    for ((pos) = hlist_entry_safe((head)->first, TYPEOF(*(pos)), member);      \
         (pos) && (((n) = (pos)->member.next), 1);                             \
         (pos) = hlist_entry_safe(n, TYPEOF(*(pos)), member))

#endif
