#ifndef _NUVIX_BITOPS_H
#define _NUVIX_BITOPS_H

/**
 * @file bitops.h
 * @brief 位操作、mask 和对齐工具。
 */

#include <nuvix/types.h>
#include <nuvix/compiler.h>
#include <nuvix/tools.h>

/** @def BIT Build an unsigned long value with bit @p n set. */
#define BIT(n)	   (1UL << (n))
/** @def BIT_U8 Build a uint8_t value with bit @p n set. */
#define BIT_U8(n)  ((uint8_t)1U << (n))
/** @def BIT_U32 Build a uint32_t value with bit @p n set. */
#define BIT_U32(n) ((uint32_t)1U << (n))
/** @def BIT_U64 Build a uint64_t value with bit @p n set. */
#define BIT_U64(n) ((uint64_t)1ULL << (n))

/**
 * @def GENMASK
 * @brief Build an unsigned long mask covering inclusive bit range h..l.
 */
#define GENMASK(h, l)                                                          \
	(((~0UL) << (l)) & (~0UL >> ((sizeof(uintptr_t) * 8) - 1 - (h))))

#define set_bit(x, n)  ((x) |= BIT(n))
#define clr_bit(x, n)  ((x) &= ~BIT(n))
#define flip_bit(x, n) ((x) ^= BIT(n))
#define test_bit(x, n) (!!((x) & BIT(n)))

#define MASK(n) (BIT(n) - 1)
/**
 * @def BITS
 * @brief Extract an inclusive bit range from an integer expression.
 * @param x Source value.
 * @param hi Highest bit index.
 * @param lo Lowest bit index.
 */
#define BITS(x, hi, lo)                                                        \
	statement_expr(static_assert((hi) >= (lo), "BITS: hi must be >= lo");  \
		       static_assert((lo) >= 0, "BITS: lo must be >= 0");      \
		       (((x) >> (lo)) & MASK((hi) - (lo) + 1));)

#define __ALIGN_MASK(x, mask) (((x) + (mask)) & ~(mask))

/**
 * @def ALIGN_UP
 * @brief Round @p x up to the next multiple of power-of-two @p a.
 */
#define ALIGN_UP(x, a)                                                         \
	statement_expr(auto _x = (x); auto _a = (a);                           \
		       if (is_constexpr(a)) BUILD_BUG_ON(!IS_POWER_OF_2(a));   \
		       __ALIGN_MASK(_x, _a - 1);)

/**
 * @def ALIGN_DOWN
 * @brief Round @p x down to a multiple of power-of-two @p a.
 */
#define ALIGN_DOWN(x, a)                                                       \
	statement_expr(auto _x = (x); auto _a = (a);                           \
		       if (is_constexpr(a)) BUILD_BUG_ON(!IS_POWER_OF_2(a));   \
		       _x & ~(_a - 1);)

/**
 * @def IS_ALIGNED
 * @brief Test whether @p x is aligned to power-of-two @p a.
 */
#define IS_ALIGNED(x, a)                                                       \
	statement_expr(auto _x = (x); auto _a = (a);                           \
		       if (is_constexpr(a)) BUILD_BUG_ON(!IS_POWER_OF_2(a));   \
		       ((_x & (_a - 1)) == 0);)

__always_inline __must_check
static inline int32_t ffz(uint64_t x)
{
	if (~x == 0)
		return 64;
	return ctzll(~x);
}

__always_inline __must_check
static inline int32_t fls(uint64_t x)
{
	if (!x)
		return 0;
	return 64UL - clzll(x);
}

#endif
