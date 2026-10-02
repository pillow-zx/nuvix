#ifndef _NUVIX_COMPILER_H
#define _NUVIX_COMPILER_H

#ifndef __has_attribute
#define __has_attribute(x) 0
#endif

#if __has_attribute(packed)
#define __packed __attribute__((__packed__))
#endif

#if __has_attribute(aligned)
#define __aligned(x) __attribute__((__aligned__(x)))
#endif

#if __has_attribute(used)
#define __used __attribute__((__used__))
#endif

#if __has_attribute(unused)
#define __unused       __attribute__((__unused__))
#define __maybe_unused __attribute__((__unused__))
#endif

#if __has_attribute(warn_unused_result)
#define __must_check __attribute__((__warn_unused_result__))
#endif

#if __has_attribute(noreturn)
#define __noreturn __attribute__((__noreturn__))
#endif

#if __has_attribute(always_inline)
#define __always_inline __attribute__((__always_inline__))
#endif

#if __has_attribute(noinline)
#define __noinline __attribute__((__noinline__))
#endif

#if __has_attribute(hot)
#define __hot __attribute__((__hot__))
#endif

#if __has_attribute(cold)
#define __cold __attribute__((__cold__))
#endif

#if __has_attribute(alias)
#define __alias(str) __attribute__((__alias__(#str)))
#endif

#if __has_attribute(pure)
#define __pure __attribute__((__pure__))
#endif

#if __has_attribute(const)
#define __const __attribute__((__const__))
#endif

#if __has_attribute(section)
#define __section(section) __attribute__((__section__(section)))
#endif

#if __has_attribute(weak)
#define __weak __attribute__((__weak__))
#endif

#if __has_attribute(format)
#define __printf(a, b) __attribute__((__format__(printf, a, b)))
#endif

#if __has_attribute(malloc)
#define __malloc __attribute__((__malloc__))
#endif

#if __has_attribute(alloc_size)
#define __alloc_size(...) __attribute__((__alloc_size__(__VA_ARGS__)))
#endif

#if __has_attribute(returns_nonnull)
#define __returns_nonnull __attribute__((__returns_nonnull__))
#endif

#if __has_attribute(nonnull)
#define __nonnull(...) __attribute__((__nonnull__(__VA_ARGS__)))
#endif

#if __has_attribute(cleanup)
#define __cleanup(func) __attribute__((__cleanup__(func)))
#endif

#if __has_attribute(deprecated)
#define __deprecated(msg) __attribute__((__deprecated__(msg)))
#endif

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#if __has_builtin(__builtin_expect)
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#if __has_builtin(__builtin_unreachable)
#define unreachable() __builtin_unreachable()
#endif

#if __has_builtin(__builtin_offsetof)
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#if __has_builtin(__builtin_return_address)
#define __return_address() __builtin_return_address(0)
#endif

#if __has_builtin(__builtin_frame_address)
#define __frame_address() __builtin_frame_address(0)
#endif

#if __has_builtin(__builtin_constant_p)
#define is_constexpr(exp) __builtin_constant_p(exp)
#endif

#if __has_builtin(__builtin_choose_expr)
#define compiletime_choose(cond, true_expr, false_expr)                        \
    __builtin_choose_expr((cond), (true_expr), (false_expr))
#endif

#if __has_builtin(__builtin_mul_overflow)
#define check_mul_overflow(n, size, bytes)                                     \
    __builtin_mul_overflow(n, size, bytes)
#endif

#if __has_builtin(__builtin_add_overflow)
#define check_add_overflow(a, b, res) __builtin_add_overflow(a, b, res)
#endif

#if __has_builtin(__builtin_sub_overflow)
#define check_sub_overflow(a, b, res) __builtin_sub_overflow(a, b, res)
#endif

#if __has_builtin(__builtin_object_size)
#define object_size(ptr, type) __builtin_object_size(ptr, type)
#endif

#endif /* ifndef _NUVIX_COMPILER_H */
