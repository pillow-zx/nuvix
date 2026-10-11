#ifndef _NUVIX_HASHTABLE_H
#define _NUVIX_HASHTABLE_H

#include <nuvix/hlist.h>
#include <nuvix/assert.h>

#define DECLARE_HASHTABLE(name, bits) struct hlist_head name[1UL << (bits)]
#define DEFINE_HASHTABLE(name, bits)                                           \
    DECLARE_HASHTABLE(name,                                                    \
                      bits) = {[0 ...((1UL << (bits)) - 1)] = HLIST_HEAD_INIT}

#define HASH_SIZE(table) (sizeof(table) / sizeof((table)[0]))
#define HASH_BITS(table)                                                       \
    ({                                                                         \
        static_assert(                                                         \
            !__builtin_types_compatible_p(TYPEOF(table), TYPEOF(&(table)[0])), \
            "hash table must be an array");                                    \
        static_assert(HASH_SIZE(table) &&                                      \
                          !(HASH_SIZE(table) & (HASH_SIZE(table) - 1)),        \
                      "hash table size must be a power of two");               \
        (unsigned int)__builtin_ctzl(HASH_SIZE(table));                        \
    })

static inline uint64_t hash_64(uint64_t value, unsigned int bits)
{
    return bits ? (value * 0x61c8864680b583ebULL) >> (64 - bits) : 0;
}

static inline void __hash_init(struct hlist_head *table, size_t size)
{
    for (size_t bucket = 0; bucket < size; ++bucket)
        INIT_HLIST_HEAD(&table[bucket]);
}

#define hash_init(table)                                                       \
    do {                                                                       \
        (void)HASH_BITS(table);                                                \
        __hash_init(table, HASH_SIZE(table));                                  \
    } while (0)

#define hash_add(table, node, key)                                             \
    hlist_add_head(node, &(table)[hash_64((uint64_t)(key), HASH_BITS(table))])

static inline void hash_del(struct hlist_node *node)
{
    hlist_del_init(node);
}

static inline bool hash_hashed(const struct hlist_node *node)
{
    return !hlist_unhashed(node);
}

static inline bool __hash_empty(const struct hlist_head *table, size_t size)
{
    for (size_t bucket = 0; bucket < size; ++bucket) {
        if (!hlist_empty(&table[bucket]))
            return false;
    }
    return true;
}

#define hash_empty(table)                                                      \
    ({                                                                         \
        (void)HASH_BITS(table);                                                \
        __hash_empty(table, HASH_SIZE(table));                                 \
    })

#define hash_for_each(table, bucket, obj, member)                              \
    for ((bucket) = 0, (obj) = NULL; !(obj) && (bucket) < HASH_SIZE(table);    \
         ++(bucket))                                                           \
        hlist_for_each_entry (obj, &(table)[bucket], member)

#define hash_for_each_safe(table, bucket, next, obj, member)                   \
    for ((bucket) = 0, (obj) = NULL; !(obj) && (bucket) < HASH_SIZE(table);    \
         ++(bucket))                                                           \
        hlist_for_each_entry_safe (obj, next, &(table)[bucket], member)

#define hash_for_each_possible(table, obj, member, key)                        \
    hlist_for_each_entry (                                                     \
        obj, &(table)[hash_64((uint64_t)(key), HASH_BITS(table))], member)

#define hash_for_each_possible_safe(table, obj, next, member, key)             \
    hlist_for_each_entry_safe (                                                \
        obj, next, &(table)[hash_64((uint64_t)(key), HASH_BITS(table))],       \
        member)

#endif
