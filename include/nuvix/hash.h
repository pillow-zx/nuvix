#ifndef _NUVIX_HASH_H
#define _NUVIX_HASH_H

/**
 * @file hash.h
 * @brief Intrusive hash table helpers built from list_head buckets.
 */

#include <nuvix/list.h>
#include <nuvix/types.h>

/**
 * @struct hash_table
 * @brief Bucket array metadata for intrusive hash tables.
 *
 * @par Fields
 * - @c buckets: Array of 2^bits bucket heads.
 * - @c bits: log2 bucket count; hashes are masked by this width.
 */
struct hash_table {
	struct list_head *buckets;
	uint32_t bits;
};

/**
 * @def HASH_TABLE_SIZE
 * @brief Convert a log2 bucket count into an actual bucket count.
 */
#define HASH_TABLE_SIZE(hash_bits) (1u << (hash_bits))

/**
 * @def HASH_TABLE_DECLARE
 * @brief Declare a bucket array and hash_table object with automatic storage.
 * @param name Base variable name.
 * @param hash_bits log2 number of buckets.
 */
#define HASH_TABLE_DECLARE(name, hash_bits)                                    \
	struct list_head name##_buckets[HASH_TABLE_SIZE(hash_bits)];           \
	struct hash_table name = {                                             \
		.buckets = name##_buckets,                                     \
		.bits = (hash_bits),                                           \
	}

/**
 * @def HASH_TABLE_DECLARE_STATIC
 * @brief Declare a static bucket array and hash_table object.
 * @param name Base variable name.
 * @param hash_bits log2 number of buckets.
 */
#define HASH_TABLE_DECLARE_STATIC(name, hash_bits)                             \
	static struct list_head name##_buckets[HASH_TABLE_SIZE(hash_bits)];    \
	static struct hash_table name = {                                      \
		.buckets = name##_buckets,                                     \
		.bits = (hash_bits),                                           \
	}

static inline void hash_table_init(struct hash_table *table)
{
	for (uint32_t i = 0; i < HASH_TABLE_SIZE(table->bits); i++)
		INIT_LIST_HEAD(&table->buckets[i]);
}

static inline struct list_head *
hash_table_bucket(struct hash_table *table, uint64_t hash)
{
	return &table->buckets[(uint32_t)hash &
			       (HASH_TABLE_SIZE(table->bits) - 1u)];
}

static inline void
hash_table_add(struct hash_table *table, uint64_t hash, struct list_head *node)
{
	list_add(node, hash_table_bucket(table, hash));
}

static inline void hash_table_del(struct list_head *node)
{
	list_del(node);
}

/**
 * @def hash_table_for_each_possible
 * @brief Iterate raw nodes in the one bucket selected by a hash value.
 * @param pos Cursor of type `struct list_head *`.
 * @param table Hash table.
 * @param hash Full hash value; low bits select the bucket.
 */
#define hash_table_for_each_possible(pos, table, hash)                         \
	list_for_each (pos, hash_table_bucket((table), (hash)))

#endif
