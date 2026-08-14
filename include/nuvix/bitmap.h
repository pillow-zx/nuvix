/**
 * @file bitmap.h
 * @brief Fixed-size bitmap helpers.
 */

#ifndef _NUVIX_BITMAP_H
#define _NUVIX_BITMAP_H

#include <nuvix/compiler.h>
#include <nuvix/tools.h>
#include <nuvix/types.h>

/**
 * @def BITMAP_WORD_BITS
 * @brief Number of bits represented by one uintptr_t bitmap word.
 */
#define BITMAP_WORD_BITS sizeof(uintptr_t) * 8U

/**
 * @def BITMAP_WORDS
 * @brief Number of uintptr_t storage words required for @p nbits bits.
 */
#define BITMAP_WORDS(nbits)                                                    \
	(((nbits) + BITMAP_WORD_BITS - 1) / BITMAP_WORD_BITS)

/**
 * @def BITMAP_BYTES
 * @brief Number of storage bytes required for @p nbits bits.
 */
#define BITMAP_BYTES(nbits) (BITMAP_WORDS(nbits) * sizeof(uintptr_t))

/**
 * @struct bitmap
 * @brief Bitmap view over caller-owned uintptr_t storage.
 *
 * @par Fields
 * - @c words: Storage words.
 * - @c nbits: Number of valid bits.
 * - @c nwords: Number of words in @ref words.
 */
struct bitmap {
	uintptr_t *words;
	size_t nbits;
	size_t nwords;
};

/**
 * @def BITMAP_DECLARE
 * @brief Declare automatic bitmap storage and a bitmap descriptor.
 * @param name Base variable name.
 * @param n Number of valid bits.
 */
#define BITMAP_DECLARE(name, n)                                                \
	uintptr_t name##_storage[BITMAP_WORDS(n)];                             \
	struct bitmap name = {                                                 \
		.words = name##_storage,                                       \
		.nbits = (n),                                                  \
		.nwords = BITMAP_WORDS(n),                                     \
	}

/**
 * @def BITMAP_DECLARE_STATIC
 * @brief Declare static bitmap storage and a bitmap descriptor.
 * @param name Base variable name.
 * @param n Number of valid bits.
 */
#define BITMAP_DECLARE_STATIC(name, n)                                         \
	static uintptr_t name##_storage[BITMAP_WORDS(n)];                      \
	static struct bitmap name = {                                          \
		.words = name##_storage,                                       \
		.nbits = (n),                                                  \
		.nwords = BITMAP_WORDS(n),                                     \
	}

__must_check
static inline size_t bitmap_word_index(size_t bit)
{
	return bit / BITMAP_WORD_BITS;
}

__must_check
static inline size_t bitmap_word_offset(size_t bit)
{
	return bit % BITMAP_WORD_BITS;
}

__must_check
static inline uintptr_t bitmap_mask_lo(size_t bits)
{
	if (bits == 0)
		return 0UL;
	if (bits >= BITMAP_WORD_BITS)
		return ~0UL;
	return ((uintptr_t)1UL << bits) - 1UL;
}

__must_check
static inline uintptr_t bitmap_mask_hi(size_t bit)
{
	return ~bitmap_mask_lo(bit);
}

__must_check
static inline uintptr_t bitmap_tail_mask(size_t nbits)
{
	const size_t tail = nbits % BITMAP_WORD_BITS;

	return tail == 0 ? ~0UL : bitmap_mask_lo(tail);
}

/**
 * @brief Bind a bitmap descriptor to caller-owned storage.
 */
__nonnull(1, 2)
static inline void bitmap_init(struct bitmap *map, uintptr_t *words,
		size_t nbits)
{
	map->words = words;
	map->nbits = nbits;
	map->nwords = BITMAP_WORDS(nbits);
}

/**
 * @brief Clear every valid bit in a bitmap.
 */
__nonnull(1)
static inline void bitmap_zero(struct bitmap *map)
{
	for (size_t i = 0; i < map->nwords; i++)
		map->words[i] = 0UL;
}

/**
 * @brief Set every valid bit in a bitmap.
 */
__nonnull(1)
static inline void bitmap_fill(struct bitmap *map)
{
	for (size_t i = 0; i < map->nwords; i++)
		map->words[i] = ~0UL;

	if (map->nwords > 0)
		map->words[map->nwords - 1] &= bitmap_tail_mask(map->nbits);
}

/**
 * @brief Set one bit. Out-of-range bits are ignored.
 */
__nonnull(1)
static inline void bitmap_set(struct bitmap *map, size_t bit)
{
	if (unlikely(bit >= map->nbits))
		return;

	map->words[bitmap_word_index(bit)] |= (uintptr_t)1UL
					      << bitmap_word_offset(bit);
}

/**
 * @brief Clear one bit. Out-of-range bits are ignored.
 */
__nonnull(1)
static inline void bitmap_clear(struct bitmap *map, size_t bit)
{
	if (unlikely(bit >= map->nbits))
		return;

	map->words[bitmap_word_index(bit)] &=
		~((uintptr_t)1UL << bitmap_word_offset(bit));
}

/**
 * @brief Test one bit. Out-of-range bits are reported as clear.
 */
__must_check __nonnull(1)
static inline bool bitmap_test(const struct bitmap *map, size_t bit)
{
	if (unlikely(bit >= map->nbits))
		return false;

	return !!(map->words[bitmap_word_index(bit)] &
		  ((uintptr_t)1UL << bitmap_word_offset(bit)));
}

/**
 * @brief Set or clear one bit according to @p value.
 */
__nonnull(1)
static inline void bitmap_assign(struct bitmap *map, size_t bit,
					      bool value)
{
	if (value)
		bitmap_set(map, bit);
	else
		bitmap_clear(map, bit);
}

/**
 * @brief Set @p count bits starting at @p start.
 *
 * The range is clipped to the valid bitmap extent.
 */
__nonnull(1)
static inline void bitmap_set_range(struct bitmap *map,
						 size_t start, size_t count)
{
	if (count == 0 || unlikely(start >= map->nbits))
		return;

	const size_t end = start + MIN(count, map->nbits - start);
	const size_t first = bitmap_word_index(start);
	const size_t last = bitmap_word_index(end - 1);
	const size_t start_bit = bitmap_word_offset(start);
	const size_t end_bit = bitmap_word_offset(end);
	const uintptr_t last_mask =
		end_bit == 0 ? ~0UL : bitmap_mask_lo(end_bit);

	if (first == last) {
		map->words[first] |= bitmap_mask_hi(start_bit) & last_mask;
		return;
	}

	map->words[first] |= bitmap_mask_hi(start_bit);

	for (size_t i = first + 1; i < last; i++)
		map->words[i] = ~0UL;

	map->words[last] |= last_mask;
}

/**
 * @brief Clear @p count bits starting at @p start.
 *
 * The range is clipped to the valid bitmap extent.
 */
__nonnull(1)
static inline void bitmap_clear_range(struct bitmap *map,
						   size_t start, size_t count)
{
	if (count == 0 || unlikely(start >= map->nbits))
		return;

	const size_t end = start + MIN(count, map->nbits - start);
	const size_t first = bitmap_word_index(start);
	const size_t last = bitmap_word_index(end - 1);
	const size_t start_bit = bitmap_word_offset(start);
	const size_t end_bit = bitmap_word_offset(end);
	const uintptr_t last_mask =
		end_bit == 0 ? ~0UL : bitmap_mask_lo(end_bit);

	if (first == last) {
		map->words[first] &= ~(bitmap_mask_hi(start_bit) & last_mask);
		return;
	}

	map->words[first] &= ~bitmap_mask_hi(start_bit);

	for (size_t i = first + 1; i < last; i++)
		map->words[i] = 0UL;

	map->words[last] &= ~last_mask;
}

/**
 * @brief Find the first set bit, or @c map->nbits when none exists.
 */
__must_check __nonnull(1)
static inline size_t bitmap_find_first_set(const struct bitmap *map)
{
	for (size_t i = 0; i < map->nwords; i++) {
		uintptr_t word = map->words[i];

		if (i + 1 == map->nwords)
			word &= bitmap_tail_mask(map->nbits);

		if (word != 0UL)
			return i * BITMAP_WORD_BITS + (size_t)ctzl(word);
	}

	return map->nbits;
}

/**
 * @brief Find the first clear bit, or @c map->nbits when none exists.
 */
__must_check __nonnull(1)
static inline size_t bitmap_find_first_zero(const struct bitmap *map)
{
	for (size_t i = 0; i < map->nwords; i++) {
		uintptr_t word = ~map->words[i];

		if (i + 1 == map->nwords)
			word &= bitmap_tail_mask(map->nbits);

		if (word != 0UL)
			return i * BITMAP_WORD_BITS + (size_t)ctzl(word);
	}

	return map->nbits;
}

/**
 * @brief Find the first set bit at or after @p start.
 */
__must_check __nonnull(1)
static inline size_t bitmap_find_next_set(const struct bitmap *map, size_t start)
{
	if (unlikely(start >= map->nbits))
		return map->nbits;

	size_t i = bitmap_word_index(start);
	const size_t offset = bitmap_word_offset(start);
	uintptr_t word = map->words[i] & bitmap_mask_hi(offset);

	if (i + 1 == map->nwords)
		word &= bitmap_tail_mask(map->nbits);

	if (word != 0UL)
		return i * BITMAP_WORD_BITS + (size_t)ctzl(word);

	for (i++; i < map->nwords; i++) {
		word = map->words[i];
		if (i + 1 == map->nwords)
			word &= bitmap_tail_mask(map->nbits);

		if (word != 0UL)
			return i * BITMAP_WORD_BITS + (size_t)ctzl(word);
	}

	return map->nbits;
}

/**
 * @brief Find the first clear bit at or after @p start.
 */
__must_check __nonnull(1)
static inline size_t bitmap_find_next_zero(const struct bitmap *map, size_t start)
{
	if (unlikely(start >= map->nbits))
		return map->nbits;

	size_t i = bitmap_word_index(start);
	const size_t offset = bitmap_word_offset(start);
	uintptr_t word = ~map->words[i] & bitmap_mask_hi(offset);

	if (i + 1 == map->nwords)
		word &= bitmap_tail_mask(map->nbits);

	if (word != 0UL)
		return i * BITMAP_WORD_BITS + (size_t)ctzl(word);

	for (i++; i < map->nwords; i++) {
		word = ~map->words[i];
		if (i + 1 == map->nwords)
			word &= bitmap_tail_mask(map->nbits);

		if (word != 0UL)
			return i * BITMAP_WORD_BITS + (size_t)ctzl(word);
	}

	return map->nbits;
}

/**
 * @brief Count set bits in the valid bitmap extent.
 */
__must_check __nonnull(1)
static inline size_t bitmap_weight(const struct bitmap *map)
{
	size_t count = 0;

	for (size_t i = 0; i < map->nwords; i++) {
		uintptr_t word = map->words[i];

		if (i + 1 == map->nwords)
			word &= bitmap_tail_mask(map->nbits);

		count += (size_t)popcountl(word);
	}

	return count;
}

/**
 * @brief Return whether every valid bit is clear.
 */
__must_check __nonnull(1)
static inline bool bitmap_empty(const struct bitmap *map)
{
	return bitmap_weight(map) == 0;
}

/**
 * @brief Return whether every valid bit is set.
 */
__must_check __nonnull(1)
static inline bool bitmap_full(const struct bitmap *map)
{
	return bitmap_weight(map) == map->nbits;
}

#endif
