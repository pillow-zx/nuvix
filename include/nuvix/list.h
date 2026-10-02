#ifndef _NUVIX_LIST_H
#define _NUVIX_LIST_H

#include <nuvix/macro.h>
#include <nuvix/ctypes.h>

struct list_head {
    struct list_head *prev;
    struct list_head *next;
};

#define LIST_INIT_HEAD(name) {&(name), &(name)}
#define LIST_HEAD(name)      struct list_head name = LIST_INIT_HEAD(name)

static inline void INIT_LIST_HEAD(struct list_head *list)
{
    WRITE_ONCE(list->prev, list);
    WRITE_ONCE(list->next, list);
}

static inline void __list_add(struct list_head *new, struct list_head *prev,
                              struct list_head *next)
{
    next->prev = new;
    new->next = next;
    new->prev = prev;
    WRITE_ONCE(prev->next, new);
}

static inline void list_add(struct list_head *new, struct list_head *head)
{
    __list_add(new, head, head->next);
}

static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
    __list_add(new, head->prev, head);
}

static inline void __list_del(struct list_head *prev, struct list_head *next)
{
    next->prev = prev;
    WRITE_ONCE(prev->next, next);
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

static inline void list_move_tail(struct list_head *list,
                                  struct list_head *head)
{
    __list_del(list->prev, list->next);
    list_add_tail(list, head);
}

static inline int list_is_first(const struct list_head *list,
                                const struct list_head *head)
{
    return list->prev == head;
}

static inline int list_is_last(const struct list_head *list,
                               const struct list_head *head)
{
    return list->next == head;
}

static inline int list_is_head(const struct list_head *list,
                               const struct list_head *head)
{
    return list == head;
}

static inline int list_empty(const struct list_head *head)
{
    return READ_ONCE(head->next) == head;
}

#define list_for_each(pos, head)                                               \
    for (pos = (head)->next; pos != (head); pos = pos->next)

#define list_for_each_safe(pos, n, head)                                       \
    for (pos = (head)->next, n = pos->next; pos != (head);                     \
         pos = n, n = pos->next)

#define list_entry(ptr, type, member) container_of(ptr, type, member)

#define list_first_entry(ptr, type, member)                                    \
    list_entry((ptr)->next, type, member)

#define list_for_each_entry(pos, head, member)                                 \
    for (pos = list_entry((head)->next, TYPEOF(*pos), member);                 \
         &pos->member != (head);                                               \
         pos = list_entry(pos->member.next, TYPEOF(*pos), member))

#define list_for_each_entry_safe(pos, n, head, member)                         \
    for (pos = list_entry((head)->next, TYPEOF(*pos), member),                 \
        n = list_entry(pos->member.next, TYPEOF(*pos), member);                \
         &pos->member != (head);                                               \
         pos = n, n = list_entry(n->member.next, TYPEOF(*n), member))

#endif /* ifndef _NUVIX_LIST_H */
