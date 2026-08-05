/**
 * @file tools.h
 * @brief 内核通用工具宏。
 */

#ifndef _CUTEOS_KERNEL_TOOLS_H
#define _CUTEOS_KERNEL_TOOLS_H

#include <kernel/compiler.h>

/**
 * @def CONCAT
 * @brief Concatenate two preprocessor tokens after expanding their arguments.
 *
 * The extra expansion layer lets a macro argument expand before it is joined
 * with the other token. This is required by IFDEF() and IFNDEF() when they
 * form the internal property token for a boolean configuration macro.
 */
#define __CONCAT_RAW(a, b) a##b
#define CONCAT(a, b) __CONCAT_RAW(a, b)

#define __CHOOSE2ND(a, b, ...)		      b
#define __MUX_WITH_COMMA(contain_comma, a, b) __CHOOSE2ND(contain_comma a, b)
#define __MUX_MACRO_PROPERTY(p, macro, a, b)                                   \
	__MUX_WITH_COMMA(CONCAT(p, macro), a, b)

#define __P_DEF_0 X,
#define __P_DEF_1 X,

#define __MUXDEF(macro, x, y)  __MUX_MACRO_PROPERTY(__P_DEF_, macro, x, y)
#define __MUXNDEF(macro, x, y) __MUX_MACRO_PROPERTY(__P_DEF_, macro, y, x)

#define __IGNORE(...)
#define __KEEP(...) __VA_ARGS__

/**
 * @def IFDEF
 * @brief Keep tokens only when a boolean macro expands to 0 or 1.
 * @param macro Object-like boolean macro to test.
 * @param ... Tokens retained when @p macro is defined to 0 or 1.
 *
 * This is a token-selection macro, not an equivalent of the preprocessor
 * #ifdef directive. The tested macro must expand to exactly 0 or 1; an
 * undefined macro and any other expansion select the empty branch.
 *
 * IFDEF() concatenates __P_DEF_ with the expanded value of @p macro. The
 * matching __P_DEF_0 or __P_DEF_1 placeholder expands to `X,`, while an
 * undefined property token does not contain a comma. __MUX_WITH_COMMA()
 * places that result before __KEEP and __IGNORE; the comma changes how
 * __CHOOSE2ND() parses its arguments, selecting __KEEP for 0 or 1 and
 * __IGNORE otherwise. The selected variadic macro either emits or discards
 * the supplied tokens.
 */
#define IFDEF(macro, ...)  __MUXDEF(macro, __KEEP, __IGNORE)(__VA_ARGS__)

/**
 * @def IFNDEF
 * @brief Keep tokens unless a boolean macro expands to 0 or 1.
 * @param macro Object-like boolean macro to test.
 * @param ... Tokens retained when @p macro is undefined or has another value.
 *
 * This is the inverse of IFDEF(). It uses the same comma-producing
 * __P_DEF_0 and __P_DEF_1 placeholders, but reverses the __KEEP and
 * __IGNORE branches before the selected variadic macro consumes the tokens.
 * Consequently, @p macro must follow the same exact 0-or-1 constraint as
 * IFDEF().
 */
#define IFNDEF(macro, ...) __MUXNDEF(macro, __KEEP, __IGNORE)(__VA_ARGS__)

/**
 * @def MMIO_READ
 * @brief Perform a volatile typed load from an MMIO address.
 */
#define MMIO_READ(type, addr)	    (*(volatile type *)(addr))

/**
 * @def MMIO_WRITE
 * @brief Perform a volatile typed store to an MMIO address.
 */
#define MMIO_WRITE(type, addr, val) (*(volatile type *)(addr) = (val))

/**
 * @def ISARR
 * @brief Compile-time assertion that an expression is an array.
 */
#define ISARR(arr, msg) static_assert(!same_type((arr), &(arr)[0]), msg)

/**
 * @def ARRLEN
 * @brief Return the number of elements in an array expression.
 *
 * The macro rejects pointer arguments at compile time by comparing the array
 * expression type with the type of its first-element pointer.
 */
#define ARRLEN(arr)                                                            \
	statement_expr(                                                        \
		ISARR(arr,                                                     \
		      "ARRLEN: argument must be an array, not an pointer");    \
		sizeof((arr)) / sizeof((arr)[0]);)

/**
 * @def BUILD_BUG_ON
 * @brief Trigger a compile-time error when @p cond is true.
 */
#define BUILD_BUG_ON(cond)	((void)sizeof(char[1 - 2 * !!(cond)]))

/**
 * @def BUILD_BUG_ON_ZERO
 * @brief Compile-time assertion expression that evaluates to zero.
 */
#define BUILD_BUG_ON_ZERO(cond) ((int)sizeof(char[1 - 2 * !!(cond)]) - 1)

/**
 * @def typecheck
 * @brief Compile-time check that an expression has an exact type.
 */
#define typecheck(type, expr)                                                  \
	statement_expr(type __dummy; typeof(expr) __dummy2;                    \
		       (void)(&__dummy == &__dummy2); 1;)

/**
 * @def typecheck_pointer
 * @brief Compile-time check that an expression has pointer-to-type type.
 */
#define typecheck_pointer(type, expr) typecheck(type *, expr)

/**
 * @def container_of
 * @brief Recover a containing object pointer from an embedded member pointer.
 * @param ptr Pointer to @p member.
 * @param type Containing object type.
 * @param member Member name inside @p type.
 *
 * A static type check verifies that @p ptr points to the selected member type
 * unless the pointer is explicitly void-typed.
 */
#define container_of(ptr, type, member)                                        \
	statement_expr(                                                        \
		static_assert(same_type(*(ptr), ((type *)0)->member) ||        \
				      same_type(*(ptr), void),                 \
			      "pointer type mismatch in container_of()");      \
		(type *)((void *)((__UINTPTR_TYPE__)(ptr) -                    \
				  offsetof(type, member)));)

/**
 * @def container_of_const
 * @brief Const-preserving variant of @ref container_of.
 */
#define container_of_const(ptr, type, member)                                  \
	statement_expr(                                                        \
		static_assert(same_type(*(ptr), ((type *)0)->member) ||        \
				      same_type(*(ptr), void),                 \
			      "pointer type mismatch in container_of()");      \
		_Generic((ptr),                                                \
			const typeof(*(ptr)) *: (const type *)((               \
				const void *)((const char *)(ptr) -            \
					      offsetof(type, member))),        \
			default: ((                                            \
				type *)((void *)((__UINTPTR_TYPE__)(ptr) -     \
						 offsetof(type, member)))));)

/**
 * @def is_constexpr
 * @brief Test whether an expression is known constant to the compiler.
 */
#define is_constexpr(expr) constant_p(expr)

/**
 * @def IS_POWER_OF_2
 * @brief Return true when @p x is a non-zero power of two.
 */
#define IS_POWER_OF_2(x) ((x) != 0 && (((x) & ((x) - 1)) == 0))

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

/**
 * @def MIN
 * @brief Type-checked minimum of two same-typed expressions.
 */
#define MIN(a, b)                                                              \
	statement_expr(                                                        \
		static_assert(                                                 \
			same_type(a, b),                                       \
			"MIN Requires both arguments to be the same type");    \
		auto _a = (a); auto _b = (b); _a < _b ? _a : _b;)


#endif
