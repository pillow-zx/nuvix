#ifndef _NUVIX_COMPILER_BUILTIN_H
#define _NUVIX_COMPILER_BUILTIN_H

/*
 * include/compiler/compiler_builtin.h - compiler builtin 宏
 */

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#if __has_builtin(__builtin_expect)
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x)   (x)
#define unlikely(x) (x)
#endif

#if __has_builtin(__builtin_unreachable)
#define unreachable() __builtin_unreachable()
#else
#define unreachable() ((void)0)
#endif

#if __has_builtin(__builtin_trap)
#define trap() __builtin_trap()
#else
#define trap()                                                                 \
	do {                                                                   \
	} while (0)
#endif

#if __has_builtin(__builtin_offsetof)
#define offsetof(type, member) __builtin_offsetof(type, member)
#else
#define offsetof(type, member) ((unsigned long)&(((type *)0)->member))
#endif

#if __has_builtin(__builtin_prefetch)
#define prefetch(x, rw, locality) __builtin_prefetch(x, rw, locality)
#else
#define prefetch(x, rw, locality)
#endif

#if __has_builtin(__builtin_return_address)
#define __return_address() __builtin_return_address(0)
#else
#define __return_address()
#endif

#if __has_builtin(__builtin_frame_address)
#define __frame_address() __builtin_frame_address(0)
#else
#define __frame_address()
#endif

#if __has_builtin(__builtin_ffs)
#define ffs(x) __builtin_ffs(x)
#else
#define ffs(x)
#endif

#if __has_builtin(__builtin_ffsl)
#define ffsl(x) __builtin_ffsl(x)
#else
#define ffsl(x)
#endif

#if __has_builtin(__builtin_ffsll)
#define ffsll(x) __builtin_ffsll(x)
#else
#define ffsl(x)
#endif

#if __has_builtin(__builtin_clz)
#define clz(x) __builtin_clz(x)
#else
#define clz(x)
#endif

#if __has_builtin(__builtin_clzl)
#define clzl(x) __builtin_clzl(x)
#else
#define clzl(x)
#endif

#if __has_builtin(__builtin_clzll)
#define clzll(x) __builtin_clzll(x)
#else
#define clzll(x)
#endif

#if __has_builtin(__builtin_ctz)
#define ctz(x) __builtin_ctz(x)
#else
#define ctz(x)
#endif

#if __has_builtin(__builtin_ctzl)
#define ctzl(x) __builtin_ctzl(x)
#else
#define ctzl(x)
#endif

#if __has_builtin(__builtin_ctzll)
#define ctzll(x) __builtin_ctzll(x)
#else
#define ctzll(x)
#endif

#if __has_builtin(__builtin_popcount)
#define popcount(x) __builtin_popcount(x)
#else
#define popcount(x)
#endif

#if __has_builtin(__builtin_popcountl)
#define popcountl(x) __builtin_popcountl(x)
#else
#define popcountl(x)
#endif

#if __has_builtin(__builtin_popcountll)
#define popcountll(x) __builtin_popcountll(x)
#else
#define popcountll(x)
#endif

#if __has_builtin(__builtin_constant_p)
#define constant_p(exp) __builtin_constant_p(exp)
#else
#define constant_p(exp)
#endif

#if __has_builtin(__builtin_choose_expr)
#define compiletime_choose(cond, true_expr, false_expr)                        \
	__builtin_choose_expr((cond), (true_expr), (false_expr))
#else
#define compiletime_choose(cond, true_expr, false_expr)
#endif

#if __has_builtin(__builtin_types_compatible_p)
#define types_compatible(a, b)                                                 \
	__builtin_types_compatible_p(__typeof__(a), __typeof__(b))
#else
#define types_compatible(a, b)
#endif

#if __has_builtin(__builtin_mul_overflow)
#define check_mul_overflow(n, size, bytes)                                     \
	__builtin_mul_overflow(n, size, bytes)
#else
#define check_mul_overflow(n, size, bytes)
#endif

#if __has_builtin(__builtin_add_overflow)
#define check_add_overflow(a, b, res) __builtin_add_overflow(a, b, res)
#else
#define check_add_overflow(a, b, res)
#endif

#if __has_builtin(__builtin_sub_overflow)
#define check_sub_overflow(a, b, res) __builtin_sub_overflow(a, b, res)
#else
#define check_sub_overflow(a, b, res)
#endif

#if __has_builtin(__builtin_object_size)
#define object_size(ptr, type) __builtin_object_size(ptr, type)
#else
#define object_size(res)
#endif

#define COMPILER_ATOMIC_RELAXED __ATOMIC_RELAXED
#define COMPILER_ATOMIC_CONSUME __ATOMIC_CONSUME
#define COMPILER_ATOMIC_ACQUIRE __ATOMIC_ACQUIRE
#define COMPILER_ATOMIC_RELEASE __ATOMIC_RELEASE
#define COMPILER_ATOMIC_ACQ_REL __ATOMIC_ACQ_REL
#define COMPILER_ATOMIC_SEQ_CST __ATOMIC_SEQ_CST

#if __has_builtin(__atomic_load_n)
#define compiler_atomic_load_n(ptr, memorder) __atomic_load_n(ptr, memorder)
#else
#error "compiler must provide __atomic_load_n"
#endif

#if __has_builtin(__atomic_store_n)
#define compiler_atomic_store_n(ptr, val, memorder)                            \
	__atomic_store_n(ptr, val, memorder)
#else
#error "compiler must provide __atomic_store_n"
#endif

#if __has_builtin(__atomic_exchange_n)
#define compiler_atomic_exchange_n(ptr, val, memorder)                         \
	__atomic_exchange_n(ptr, val, memorder)
#else
#error "compiler must provide __atomic_exchange_n"
#endif

#if __has_builtin(__atomic_fetch_add)
#define compiler_atomic_fetch_add_n(ptr, val, memorder)                        \
	__atomic_fetch_add(ptr, val, memorder)
#else
#error "compiler must provide __atomic_fetch_add"
#endif

#if __has_builtin(__atomic_add_fetch)
#define compiler_atomic_add_fetch_n(ptr, val, memorder)                        \
	__atomic_add_fetch(ptr, val, memorder)
#else
#error "compiler must provide __atomic_add_fetch"
#endif

#if __has_builtin(__atomic_fetch_sub)
#define compiler_atomic_fetch_sub_n(ptr, val, memorder)                        \
	__atomic_fetch_sub(ptr, val, memorder)
#else
#error "compiler must provide __atomic_fetch_sub"
#endif

#if __has_builtin(__atomic_sub_fetch)
#define compiler_atomic_sub_fetch_n(ptr, val, memorder)                        \
	__atomic_sub_fetch(ptr, val, memorder)
#else
#error "compiler must provide __atomic_sub_fetch"
#endif

#if __has_builtin(__atomic_fetch_and)
#define compiler_atomic_fetch_and_n(ptr, val, memorder)                        \
	__atomic_fetch_and(ptr, val, memorder)
#else
#error "compiler must provide __atomic_fetch_and"
#endif

#if __has_builtin(__atomic_and_fetch)
#define compiler_atomic_and_fetch_n(ptr, val, memorder)                        \
	__atomic_and_fetch(ptr, val, memorder)
#else
#error "compiler must provide __atomic_and_fetch"
#endif

#if __has_builtin(__atomic_fetch_or)
#define compiler_atomic_fetch_or_n(ptr, val, memorder)                         \
	__atomic_fetch_or(ptr, val, memorder)
#else
#error "compiler must provide __atomic_fetch_or"
#endif

#if __has_builtin(__atomic_or_fetch)
#define compiler_atomic_or_fetch_n(ptr, val, memorder)                         \
	__atomic_or_fetch(ptr, val, memorder)
#else
#error "compiler must provide __atomic_or_fetch"
#endif

#if __has_builtin(__atomic_fetch_xor)
#define compiler_atomic_fetch_xor_n(ptr, val, memorder)                        \
	__atomic_fetch_xor(ptr, val, memorder)
#else
#error "compiler must provide __atomic_fetch_xor"
#endif

#if __has_builtin(__atomic_xor_fetch)
#define compiler_atomic_xor_fetch_n(ptr, val, memorder)                        \
	__atomic_xor_fetch(ptr, val, memorder)
#else
#error "compiler must provide __atomic_xor_fetch"
#endif

#if __has_builtin(__atomic_compare_exchange_n)
#define compiler_atomic_compare_exchange_n(ptr, expected, desired, weak,       \
					   succ_mo, fail_mo)                   \
	__atomic_compare_exchange_n(ptr, expected, desired, weak, succ_mo,     \
				    fail_mo)
#else
#error "compiler must provide __atomic_compare_exchange_n"
#endif

#if __has_builtin(__atomic_thread_fence)
#define compiler_atomic_thread_fence(memorder) __atomic_thread_fence(memorder)
#else
#error "compiler must provide __atomic_thread_fence"
#endif

#if __has_builtin(__atomic_signal_fence)
#define compiler_atomic_signal_fence(memorder) __atomic_signal_fence(memorder)
#else
#error "compiler must provide __atomic_signal_fence"
#endif

#if __has_builtin(__atomic_always_lock_free)
#define compiler_atomic_always_lock_free(size, ptr)                            \
	__atomic_always_lock_free(size, ptr)
#else
#error "compiler must provide __atomic_always_lock_free"
#endif

#if __has_builtin(__builtin_memcpy)
#define memcpy(dst, src, n) __builtin_memcpy((dst), (src), (n))
#else
extern void *memcpy(void *restrict dst, const void *restrict src,
		    unsigned long n);
#define memcpy(dst, src, n) memcpy(dst, src, n)
#endif

#if __has_builtin(__builtin_memset)
#define memset(dst, c, n) __builtin_memset((dst), (c), (n))
#else
extern void *memset(void *dst, int c, unsigned long n);
#define memset(dst, c, n) memset((dst), (c), (n))
#endif

#if __has_builtin(__builtin_memcmp)
#define memcmp(lhs, rhs, n) __builtin_memcmp((lhs), (rhs), (n))
#else
extern int memcmp(const void *lsh, const void *rhs, unsigned long n);
#define memcmp(lhs, rhs, n) memcmp((lhs), (rhs), (n))
#endif

#if __has_builtin(__builtin_memmove)
#define memmove(dst, src, n) __builtin_memmove((dst), (src), (n))
#else
extern void *memmove(void *dst, const void *src, unsigned long n);
#define memmove(dst, src, n) memmove((dst), (src), (n))
#endif

#if __has_builtin(__builtin_strlen)
#define strlen(s) __builtin_strlen((s))
#else
extern unsigned long strlen(const char *s);
#define strlen(s) strlen((s))
#endif

#if __has_builtin(__builtin_strnlen)
#define strnlen(s, maxlen) __builtin_strnlen((s), (maxlen))
#else
extern unsigned long strnlen(const char *s, const unsigned long maxlen);
#define strnlen(s, maxlen) strnlen(s, maxlen)
#endif

#if __has_builtin(__builtin_strcmp)
#define strcmp(lhs, rhs) __builtin_strcmp((lhs), (rhs))
#else
extern int strcmp(const char *lhs, const char *rhs);
#define strcmp(lhs, rhs) strcmp(lhs, rhs)
#endif

#if __has_builtin(__builtin_strncmp)
#define strncmp(lhs, rhs, n) __builtin_strncmp((lhs), (rhs), (n))
#else
extern int strncmp(const char *lhs, const char *rhs, unsigned long n);
#define strncmp(lhs, rhs, n) strncmp((lhs), (rhs), (n))
#endif

#if __has_builtin(__builtin_strcpy)
#define strcpy(dst, src) __builtin_strcpy((dst), (src))
#else
extern char *strcpy(char *restrict dst, const char *restrict src);
#define strcpy(dst, src) strcpy((dst), (src))
#endif

#if __has_builtin(__builtin_strncpy)
#define strncpy(dst, src, n) __builtin_strncpy((dst), (src), (n))
#else
extern char *strncpy(char *restrict dst, const char *restrict src,
		     unsigned long n);
#define strncpy(dst, src, n) strncpy((dst), (src), (n))
#endif

#if __has_builtin(__builtin_strchr)
#define strchr(s, c) __builtin_strchr((s), (c))
#else
extern char *strchr(const char *s, int c);
#define strchr(s, c) strchr((s), (c))
#endif

#if __has_builtin(__builtin_strrchr)
#define strrchr(s, c) __builtin_strrchr((s), (c))
#else
extern char *strrchr(const char *s, int c);
#define strrchr(s, c) strrchr(s, c)
#endif

#endif
