#ifndef _NUVIX_HASHTABLE_H
#define _NUVIX_HASHTABLE_H

/**
 * @file hashtable.h
 * @brief Fixed-size intrusive hash tables built from hlist buckets.
 */

#include <nuvix/hlist.h>
#include <nuvix/types.h>

struct hash_table {
	struct hlist_head *buckets;
	uint32_t bits;
};

#define HASH_TABLE_SIZE(hash_bits) (1u << (hash_bits))

#define HASH_TABLE_DECLARE(name, hash_bits)                                    \
	struct hlist_head name##_buckets[HASH_TABLE_SIZE(hash_bits)];          \
	struct hash_table name = {                                             \
		.buckets = name##_buckets,                                     \
		.bits = (hash_bits),                                           \
	}

#define HASH_TABLE_DECLARE_STATIC(name, hash_bits)                             \
	static struct hlist_head name##_buckets[HASH_TABLE_SIZE(hash_bits)];   \
	static struct hash_table name = {                                      \
		.buckets = name##_buckets,                                     \
		.bits = (hash_bits),                                           \
	}

__always_inline __nonnull(1)
static inline void hash_table_init(struct hash_table *table)
{
	for (uint32_t i = 0; i < HASH_TABLE_SIZE(table->bits); i++)
		INIT_HLIST_HEAD(&table->buckets[i]);
}

__always_inline __must_check __pure __nonnull(1)
static inline struct hlist_head *hash_table_bucket(struct hash_table *table,
						   uint64_t hash)
{
	return &table->buckets[(uint32_t)hash &
			       (HASH_TABLE_SIZE(table->bits) - 1u)];
}

__always_inline __nonnull(1, 3)
static inline void hash_table_add(struct hash_table *table, uint64_t hash,
				  struct hlist_node *node)
{
	hlist_add_head(node, hash_table_bucket(table, hash));
}

__always_inline __nonnull(1)
static inline void hash_table_del(struct hlist_node *node)
{
	hlist_del_init(node);
}

#define hash_table_for_each_possible(pos, table, hash)                         \
	hlist_for_each((pos), hash_table_bucket((table), (hash)))

#define hash_table_for_each_possible_entry(pos, table, hash, member)           \
	hlist_for_each_entry((pos), hash_table_bucket((table), (hash)), member)

#endif
