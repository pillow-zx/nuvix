#ifndef _CUTEOS_KERNEL_COMPILER_H
#define _CUTEOS_KERNEL_COMPILER_H

/**
 * @file compiler.h
 * @brief Compiler extension aliases used by kernel headers.
 */

#include <compiler/compiler_attribute.h>
#include <compiler/compiler_builtin.h>

/** @def statement_expr Wrap GNU statement-expression syntax. */
#define statement_expr(...)	 __extension__({__VA_ARGS__})
/** @def same_type Test compile-time type compatibility. */
#define same_type(a, b)		 types_compatible(a, b)

#define static_assert(cond, ...) _Static_assert(cond, __VA_ARGS__)

#define auto			 __auto_type

#define alignof(type)		_Alignof(type)

#endif
