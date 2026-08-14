#ifndef _NUVIX_CLEANUP_H
#define _NUVIX_CLEANUP_H

/**
 * @file cleanup.h
 * @brief GNU cleanup-attribute ownership and scope-guard helpers.
 */

#include <nuvix/compiler.h>
#include <nuvix/types.h>

/** @def __PASTE Token-paste two already-expanded macro arguments. */
#define __PASTE(a, b)	    a##b
/** @def __PASTE2 Expand arguments, then token-paste them. */
#define __PASTE2(a, b)	    __PASTE(a, b)
/** @def __UNIQUE_ID Build a unique identifier from prefix and __COUNTER__. */
#define __UNIQUE_ID(prefix) __PASTE2(prefix, __COUNTER__)

/**
 * @def CLEANUP_DEFINE
 * @brief Define a cleanup callback usable with @ref __cleanup_with.
 * @param _name Cleanup name suffix.
 * @param _type Variable type passed to the cleanup callback.
 * @param _cleanup Code executed with `_T` holding the variable value.
 *
 * This macro hides the compiler-required `void *` cleanup callback shape and
 * re-exports a typed local variable named `_T` inside @p _cleanup.
 */
#define CLEANUP_DEFINE(_name, _type, _cleanup)                                 \
        __always_inline                                                        \
	static inline void __cleanup_##_name(void *p)                          \
	{                                                                      \
		_type _T = *(_type *)p;                                        \
		_cleanup;                                                      \
	}

/**
 * @def __cleanup_with
 * @brief Attach a cleanup callback declared by @ref CLEANUP_DEFINE.
 */
#define __cleanup_with(_name) __cleanup(__cleanup_##_name)

/**
 * @def __get_and_null
 * @brief Read an lvalue and replace it with a null/sentinel value.
 */
#define __get_and_null(p, nullvalue)                                           \
	statement_expr(auto __ptr = &(p); auto __val = *__ptr;                 \
		       *__ptr = (nullvalue); __val;)
__always_inline __must_check
static inline uintptr_t __cleanup_must_check(const volatile void *val)
{
	return (uintptr_t)val;
}

/**
 * @def cleanup_take_ptr
 * @brief Transfer pointer ownership out of a cleanup-managed variable.
 * @param p Pointer lvalue to read and set to NULL.
 */
#define cleanup_take_ptr(p)                                                    \
	((typeof(p))__cleanup_must_check(                                      \
		(const volatile void *)__get_and_null(p, NULL)))

/**
 * @def cleanup_return_ptr
 * @brief Return a pointer while suppressing the variable's cleanup action.
 */
#define cleanup_return_ptr(p) return cleanup_take_ptr(p)

/**
 * @def cleanup_forget_ptr
 * @brief Suppress cleanup for a pointer without returning it.
 */
#define cleanup_forget_ptr(p) ((void)__get_and_null(p, NULL))

/**
 * @def SCOPE_DEFINE
 * @brief Define a typed RAII-style scope helper.
 * @param _name Scope helper name.
 * @param _type Stored guard type.
 * @param exit_expr Code executed at scope exit with `_T` as guard value.
 * @param init_expr Expression that builds the guard value.
 * @param init_args Parameters accepted by the generated init function.
 */
#define SCOPE_DEFINE(_name, _type, exit_expr, init_expr, init_args...)         \
	typedef _type scope_##_name##_t;                                       \
	__always_inline                                                        \
	static inline void scope_##_name##_exit(_type *p)                      \
	{                                                                      \
		_type _T = *p;                                                 \
		exit_expr;                                                     \
	}                                                                      \
	__always_inline							       \
	static inline _type scope_##_name##_init(init_args)                    \
	{                                                                      \
		_type t = init_expr;                                           \
		return t;                                                      \
	}

/**
 * @def SCOPE_EXTEND
 * @brief Define another initializer that reuses an existing scope exit hook.
 */
#define SCOPE_EXTEND(_name, ext, init_expr, init_args...)                      \
	typedef scope_##_name##_t scope_##_name##ext##_t;                      \
        __always_inline                                                        \
	static inline void scope_##_name##ext##_exit(                          \
		scope_##_name##ext##_t *p)                                     \
	{                                                                      \
		scope_##_name##_exit(p);                                       \
	}                                                                      \
        __always_inline                                                        \
	static inline scope_##_name##_t scope_##_name##ext##_init(             \
		init_args)                                                     \
	{                                                                      \
		scope_##_name##_t t = init_expr;                               \
		return t;                                                      \
	}

/**
 * @def SCOPE_VAR
 * @brief Declare a cleanup-managed scope variable initialized by arguments.
 */
#define SCOPE_VAR(_name, var)                                                  \
	scope_##_name##_t var __cleanup(scope_##_name##_exit) =                \
		scope_##_name##_init

/**
 * @def SCOPE_VAR_INIT
 * @brief Declare a cleanup-managed scope variable from an explicit value.
 */
#define SCOPE_VAR_INIT(_name, var, init_expr)                                  \
	scope_##_name##_t var __cleanup(scope_##_name##_exit) = (init_expr)

/**
 * @def SCOPE_GUARD_DEFINE
 * @brief Define a scope guard whose init path locks and exit path unlocks.
 */
#define SCOPE_GUARD_DEFINE(_name, _type, _lock, _unlock)                       \
	SCOPE_DEFINE(_name, _type, _unlock,                                    \
		     statement_expr(_type _T = _T_init; _lock; _T;),           \
		     _type _T_init)

/**
 * @def scope_guard
 * @brief Instantiate an unnamed scope guard for the current lexical scope.
 */
#define scope_guard(_name) SCOPE_VAR(_name, __UNIQUE_ID(scope_guard_))

/**
 * @def __with_scope
 * @brief Build a single-iteration for/if block with cleanup on exit.
 */
#define __with_scope(_name, var, label, args...)                               \
	for (SCOPE_VAR(_name, var)(args);; ({ goto label; }))                  \
		if (0) {                                                       \
		label:                                                         \
			break;                                                 \
		} else

/**
 * @def with_guard
 * @brief Execute a block while a named scope guard is active.
 */
#define with_guard(_name, args...)                                             \
	__with_scope(_name, __UNIQUE_ID(scope_), __UNIQUE_ID(label_), args)

#endif
