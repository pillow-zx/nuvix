#ifndef _NUVIX_MATH_H
#define _NUVIX_MATH_H

/**
 * @file math.h
 * @brief Numeric, alignment, and checked-arithmetic helpers.
 */

#include <nuvix/compiler.h>
#include <nuvix/tools.h>

/**
 * @def IS_POWER_OF_2
 * @brief Return true when @p x is a non-zero power of two.
 */
#define IS_POWER_OF_2(x) ((x) != 0 && (((x) & ((x) - 1)) == 0))

/**
 * @def MIN
 * @brief Type-checked minimum of two same-typed expressions.
 */
#define MIN(a, b)                                                              \
	statement_expr(                                                        \
		static_assert(                                                 \
			same_type(a, b),                                       \
			"MIN requires both arguments to be the same type");    \
		auto _a = (a); auto _b = (b); _a < _b ? _a : _b;)

/**
 * @def MAX
 * @brief Type-checked maximum of two same-typed expressions.
 */
#define MAX(a, b)                                                              \
	statement_expr(                                                        \
		static_assert(                                                 \
			same_type(a, b),                                       \
			"MAX requires both arguments to be the same type");    \
		auto _a = (a); auto _b = (b); _a > _b ? _a : _b;)

/** @def CLAMP Restrict a value to an inclusive interval. */
#define CLAMP(value, low, high)                                                \
	statement_expr(                                                        \
		auto _value = (value); auto _low = (low); auto _high = (high); \
		static_assert(same_type(_value, _low) &&                       \
				      same_type(_value, _high),                \
			      "CLAMP arguments must have the same type");      \
		_value < _low ? _low : (_value > _high ? _high : _value);)

#define __ALIGN_MASK(x, mask) (((x) + (mask)) & ~(mask))

/**
 * @def ALIGN_UP
 * @brief Round @p x up to the next power-of-two multiple of @p a.
 */
#define ALIGN_UP(x, a)                                                         \
	statement_expr(auto _x = (x); auto _a = (a);                           \
		       if (is_constexpr(a)) BUILD_BUG_ON(!IS_POWER_OF_2(a));   \
		       __ALIGN_MASK(_x, _a - 1);)

/**
 * @def ALIGN_DOWN
 * @brief Round @p x down to a power-of-two multiple of @p a.
 */
#define ALIGN_DOWN(x, a)                                                       \
	statement_expr(auto _x = (x); auto _a = (a);                           \
		       if (is_constexpr(a)) BUILD_BUG_ON(!IS_POWER_OF_2(a));   \
		       _x & ~(_a - 1);)

/**
 * @def IS_ALIGNED
 * @brief Test whether @p x is aligned to a power-of-two @p a.
 */
#define IS_ALIGNED(x, a)                                                       \
	statement_expr(auto _x = (x); auto _a = (a);                           \
		       if (is_constexpr(a)) BUILD_BUG_ON(!IS_POWER_OF_2(a));   \
		       ((_x & (_a - 1)) == 0);)

#define IS_ALIGNED2(x) IS_ALIGNED(x, sizeof(uint16_t))
#define IS_ALIGNED4(x) IS_ALIGNED(x, sizeof(uint32_t))
#define IS_ALIGNED8(x) IS_ALIGNED(x, sizeof(uint64_t))

#define ALIGNMENT_OF(x)                                                        \
	(IS_ALIGNED8((x)) ? 8 : IS_ALIGNED4((x)) ? 4 : IS_ALIGNED2((x)) ? 2 : 1)

#define ALIGNMENT_OF2(x, y)                                                    \
	(IS_ALIGNED8((x)) && IS_ALIGNED8((y))	? 8                            \
	 : IS_ALIGNED4((x)) && IS_ALIGNED4((y)) ? 4                            \
	 : IS_ALIGNED2((x)) && IS_ALIGNED2((y)) ? 2                            \
						: 1)

/**
 * @def DIV_ROUND_UP
 * @brief Divide two positive values and round the result up.
 */
#define DIV_ROUND_UP(n, d)                                                     \
	statement_expr(auto _n = (n); auto _d = (d); _n / _d + !!(_n % _d);)

/* The compiler wrappers remain in compiler_builtin.h; expose them with the
 * numeric helpers instead of duplicating compiler-specific implementations. */

#endif
