#ifndef _NUVIX_COMPILER_ATTRIBUTE_H
#define _NUVIX_COMPILER_ATTRIBUTE_H

/*
 * include/compiler/compiler_attribute.h - compiler attribute 宏
 */

#ifndef __has_attribute
#define __has_attribute(x) 0
#endif

#if defined(__GNUC__) || defined(__clang__)
#if __has_attribute(packed)
#define __packed	   __attribute__((__packed__))
#else
#define __packed
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#if __has_attribute(aligned)
#define __aligned(x)	   __attribute__((__aligned__(x)))
#else
#define __aligned(x)
#endif
#endif


#if defined(__GNUC__) || defined(__clang__)
#if __has_attribute(used)
#define __used		   __attribute__((__used__))
#else
#define __used
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#if __has_attribute(unused)
#define __unused	   __attribute__((__unused__))
#define __maybe_unused	   __attribute__((__unused__))
#else
#define __unused
#define __maybe_unused
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#if __has_attribute(warn_unused_result)
#define __must_check	   __attribute__((__warn_unused_result__))
#else
#define __must_check
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#if __has_attribute(noreturn)
#define __noreturn	   __attribute__((__noreturn__))
#else
#define __noreturn
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#if __has_attribute(always_inline)
#define __always_inline	   __attribute__((__always_inline__))
#else
#define __always_inline
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#if __has_attribute(noinline)
#define __noinline	   __attribute__((__noinline__))
#else
#define __noinline
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#if __has_attribute(hot)
#define __hot		   __attribute__((__hot__))
#else
#define __hot
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#if __has_attribute(cold)
#define __cold		   __attribute__((__cold__))
#else
#define __cold
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#if __has_attribute(alias)
#define __alias(str)	   __attribute__((__alias__(#str)))
#else
#define __alias(str)
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#if __has_attribute(pure)
#define __pure		   __attribute__((__pure__))
#else
#define __pure
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#if __has_attribute(const)
#define __const		   __attribute__((__const__))
#else
#define __const
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#if __has_attribute(section)
#define __section(section) __attribute__((__section__(section)))
#else
#define __section(section)
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#if __has_attribute(weak)
#define __weak		   __attribute__((__weak__))
#else
#define __weak
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#if __has_attribute(format)
#define __printf(a, b)	   __attribute__((__format__(printf, a, b)))
#else
#define __printf(a, b)
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#if __has_attribute(malloc)
#define __malloc           __attribute__((__malloc__))
#else
#define __malloc
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#if __has_attribute(alloc_size)
#define __alloc_size(...)       __attribute__((__alloc_size__(__VA_ARGS__)))
#else
#define __alloc_size(...)
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#if __has_attribute(returns_nonnull)
#define __returns_nonnull __attribute__((__returns_nonnull__))
#else
#define __returns_nonnull
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#if __has_attribute(nonnull)
#define __nonnull(...) __attribute__((__nonnull__(__VA_ARGS__)))
#else
#define __nonnull(...)
#endif
#endif

#if defined(__GNUC__) || defined (__clang__)
#if __has_attribute(access)
#define __access(mode, ref, size)                                              \
	__attribute__((__access__(mode, ref, size)))
#define __access_no_size(mode, ref) __attribute__((__access__(mode, ref)))
#else
#define __access(mode, ref, size)
#define __access_no_size(mode, ref)
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#if __has_attribute(cleanup)
#define __cleanup(func) __attribute__((__cleanup__(func)))
#else
#define __cleanup(func);
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#if __has_attribute(deprecated)
#define __deprecated(msg) __attribute__((__deprecated__(msg)))
#else
#define __deprecated(msg);
#endif
#endif

#endif
