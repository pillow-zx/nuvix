#ifndef _NUVIX_ATOMIC_H
#define _NUVIX_ATOMIC_H

#include <nuvix/compiler.h>
#include <nuvix/types.h>

/* Keep compiler builtin details behind the kernel atomic interface. */
#define ATOMIC_ORDER_RELAXED COMPILER_ATOMIC_RELAXED
#define ATOMIC_ORDER_CONSUME COMPILER_ATOMIC_CONSUME
#define ATOMIC_ORDER_ACQUIRE COMPILER_ATOMIC_ACQUIRE
#define ATOMIC_ORDER_RELEASE COMPILER_ATOMIC_RELEASE
#define ATOMIC_ORDER_ACQ_REL COMPILER_ATOMIC_ACQ_REL
#define ATOMIC_ORDER_SEQ_CST COMPILER_ATOMIC_SEQ_CST

#define ATOMIC_DEFINE_TYPE(name, value_type)                                   \
	typedef struct {                                                       \
		value_type counter;              \
	} name

ATOMIC_DEFINE_TYPE(atomic_t, int32_t);
ATOMIC_DEFINE_TYPE(atomic64_t, int64_t);
ATOMIC_DEFINE_TYPE(atomic_isize_t, isize);

#undef ATOMIC_DEFINE_TYPE

static_assert(compiler_atomic_always_lock_free(sizeof(int32_t), 0),
	      "atomic_t must be lock-free");
static_assert(compiler_atomic_always_lock_free(sizeof(int64_t), 0),
	      "atomic64_t must be lock-free");
static_assert(compiler_atomic_always_lock_free(sizeof(isize), 0),
	      "atomic_long_t must be lock-free");

#define ATOMIC_INIT(value)	{.counter = (value)}
#define ATOMIC64_INIT(value)	{.counter = (value)}
#define ATOMIC_LONG_INIT(value) {.counter = (value)}

/*
 * These primitives are the escape hatch for an operation or memory order
 * that does not have a named helper.  The compiler validates the order:
 * loads accept relaxed/consume/acquire/seq_cst, stores accept
 * relaxed/release/seq_cst, and read-modify-write operations accept all
 * orders except consume.
 */
#define atomic_load_order(v, order) compiler_atomic_load_n(&(v)->counter, order)
#define atomic_store_order(v, value, order)                                    \
	compiler_atomic_store_n(&(v)->counter, value, order)
#define atomic_exchange_order(v, value, order)                                 \
	compiler_atomic_exchange_n(&(v)->counter, value, order)
#define atomic_compare_exchange_order(v, expected, desired, weak, success,     \
				      failure)                                 \
	compiler_atomic_compare_exchange_n(&(v)->counter, expected, desired,   \
					   weak, success, failure)

#define atomic_fetch_add_order(v, value, order)                                \
	compiler_atomic_fetch_add_n(&(v)->counter, value, order)
#define atomic_add_fetch_order(v, value, order)                                \
	compiler_atomic_add_fetch_n(&(v)->counter, value, order)
#define atomic_fetch_sub_order(v, value, order)                                \
	compiler_atomic_fetch_sub_n(&(v)->counter, value, order)
#define atomic_sub_fetch_order(v, value, order)                                \
	compiler_atomic_sub_fetch_n(&(v)->counter, value, order)
#define atomic_fetch_and_order(v, value, order)                                \
	compiler_atomic_fetch_and_n(&(v)->counter, value, order)
#define atomic_and_fetch_order(v, value, order)                                \
	compiler_atomic_and_fetch_n(&(v)->counter, value, order)
#define atomic_fetch_or_order(v, value, order)                                 \
	compiler_atomic_fetch_or_n(&(v)->counter, value, order)
#define atomic_or_fetch_order(v, value, order)                                 \
	compiler_atomic_or_fetch_n(&(v)->counter, value, order)
#define atomic_fetch_xor_order(v, value, order)                                \
	compiler_atomic_fetch_xor_n(&(v)->counter, value, order)
#define atomic_xor_fetch_order(v, value, order)                                \
	compiler_atomic_xor_fetch_n(&(v)->counter, value, order)
#define atomic_fetch_andnot_order(v, value, order)                             \
	compiler_atomic_fetch_and_n(&(v)->counter, ~(value), order)
#define atomic_andnot_fetch_order(v, value, order)                             \
	compiler_atomic_and_fetch_n(&(v)->counter, ~(value), order)

#define atomic_thread_fence(order) compiler_atomic_thread_fence(order)
#define atomic_signal_fence(order) compiler_atomic_signal_fence(order)

#define ATOMIC_DEFINE_INIT_API(prefix, atomic_type, value_type)                \
	__always_inline							       \
	static inline void prefix##_init(atomic_type *v, value_type value)     \
	{                                                                      \
		compiler_atomic_store_n(&v->counter, value,                    \
					ATOMIC_ORDER_RELAXED);                 \
	}

#define ATOMIC_DEFINE_ACCESS_API(prefix, atomic_type, value_type)              \
	__always_inline          					       \
	static inline value_type prefix##_read_relaxed(const atomic_type *v)   \
	{                                                                      \
		return compiler_atomic_load_n(&v->counter,                     \
					      ATOMIC_ORDER_RELAXED);           \
	}                                                                      \
	__always_inline                                                        \
	static inline value_type prefix##_read_consume(const atomic_type *v)   \
	{                                                                      \
		return compiler_atomic_load_n(&v->counter,                     \
					      ATOMIC_ORDER_CONSUME);           \
	}								       \
	__always_inline                                                        \
	static inline value_type prefix##_read_acquire(const atomic_type *v)   \
	{                                                                      \
		return compiler_atomic_load_n(&v->counter,                     \
					      ATOMIC_ORDER_ACQUIRE);           \
	}                                                                      \
	__always_inline							       \
	static inline value_type prefix##_read_seq_cst(const atomic_type *v)   \
	{                                                                      \
		return compiler_atomic_load_n(&v->counter,                     \
					      ATOMIC_ORDER_SEQ_CST);           \
	}								       \
	__always_inline							       \
	static inline value_type prefix##_read(const atomic_type *v)	       \
	{                                                                      \
		return prefix##_read_seq_cst(v);                               \
	}								       \
	__always_inline							       \
	static inline void prefix##_set_relaxed(atomic_type *v,		       \
							 value_type value)     \
	{                                                                      \
		compiler_atomic_store_n(&v->counter, value,                    \
					ATOMIC_ORDER_RELAXED);                 \
	}								       \
	__always_inline							       \
	static inline void prefix##_set_release(atomic_type *v,		       \
							 value_type value)     \
	{                                                                      \
		compiler_atomic_store_n(&v->counter, value,                    \
					ATOMIC_ORDER_RELEASE);                 \
	}								       \
	__always_inline							       \
	static inline void prefix##_set_seq_cst(atomic_type *v,		       \
							 value_type value)     \
	{                                                                      \
		compiler_atomic_store_n(&v->counter, value,                    \
					ATOMIC_ORDER_SEQ_CST);                 \
	}								       \
	__always_inline							       \
	static inline void prefix##_set(atomic_type *v, value_type value)      \
	{                                                                      \
		prefix##_set_seq_cst(v, value);                                \
	}								       \
	__always_inline							       \
	static inline value_type prefix##_xchg_relaxed(			       \
			atomic_type *v,	value_type value)                      \
	{                                                                      \
		return compiler_atomic_exchange_n(&v->counter, value,          \
						  ATOMIC_ORDER_RELAXED);       \
	}								       \
	__always_inline							       \
	static inline value_type prefix##_xchg_acquire(			       \
		atomic_type *v, value_type value)                              \
	{                                                                      \
		return compiler_atomic_exchange_n(&v->counter, value,          \
						  ATOMIC_ORDER_ACQUIRE);       \
	}								       \
	__always_inline							       \
	static inline value_type prefix##_xchg_release(			       \
		atomic_type *v, value_type value)                              \
	{                                                                      \
		return compiler_atomic_exchange_n(&v->counter, value,          \
						  ATOMIC_ORDER_RELEASE);       \
	}								       \
	__always_inline                                                        \
	static inline value_type prefix##_xchg_acq_rel(                        \
		atomic_type *v, value_type value)                              \
	{                                                                      \
		return compiler_atomic_exchange_n(&v->counter, value,          \
						  ATOMIC_ORDER_ACQ_REL);       \
	}								       \
	__always_inline							       \
	static inline value_type prefix##_xchg_seq_cst(                        \
		atomic_type *v, value_type value)                              \
	{                                                                      \
		return compiler_atomic_exchange_n(&v->counter, value,          \
						  ATOMIC_ORDER_SEQ_CST);       \
	}								       \
	__always_inline							       \
	static inline value_type prefix##_xchg(atomic_type *v,		       \
							value_type value)      \
	{                                                                      \
		return prefix##_xchg_seq_cst(v, value);                        \
	}

#define ATOMIC_DEFINE_CMPXCHG_ORDER(prefix, atomic_type, value_type, suffix,   \
				    success_order, failure_order)              \
        __always_inline                                                        \
	static inline value_type prefix##_cmpxchg_##suffix(                    \
		atomic_type *v, value_type old, value_type desired)            \
	{                                                                      \
		compiler_atomic_compare_exchange_n(&v->counter, &old, desired, \
						   false, success_order,       \
						   failure_order);             \
		return old;                                                    \
	}                                                                      \
        __always_inline                                                        \
	static inline bool prefix##_try_cmpxchg_##suffix(                      \
		atomic_type *v, value_type *old, value_type desired)           \
	{                                                                      \
		return compiler_atomic_compare_exchange_n(                     \
			&v->counter, old, desired, false, success_order,       \
			failure_order);                                        \
	}

#define ATOMIC_DEFINE_CMPXCHG_API(prefix, atomic_type, value_type)             \
	ATOMIC_DEFINE_CMPXCHG_ORDER(prefix, atomic_type, value_type, relaxed,  \
				    ATOMIC_ORDER_RELAXED,                      \
				    ATOMIC_ORDER_RELAXED)                      \
	ATOMIC_DEFINE_CMPXCHG_ORDER(prefix, atomic_type, value_type, acquire,  \
				    ATOMIC_ORDER_ACQUIRE,                      \
				    ATOMIC_ORDER_RELAXED)                      \
	ATOMIC_DEFINE_CMPXCHG_ORDER(prefix, atomic_type, value_type, release,  \
				    ATOMIC_ORDER_RELEASE,                      \
				    ATOMIC_ORDER_RELAXED)                      \
	ATOMIC_DEFINE_CMPXCHG_ORDER(prefix, atomic_type, value_type, acq_rel,  \
				    ATOMIC_ORDER_ACQ_REL,                      \
				    ATOMIC_ORDER_ACQUIRE)                      \
	ATOMIC_DEFINE_CMPXCHG_ORDER(prefix, atomic_type, value_type, seq_cst,  \
				    ATOMIC_ORDER_SEQ_CST,                      \
				    ATOMIC_ORDER_SEQ_CST)                      \
        __always_inline                                                        \
	static inline value_type prefix##_cmpxchg(                             \
		atomic_type *v, value_type old, value_type desired)            \
	{                                                                      \
		return prefix##_cmpxchg_seq_cst(v, old, desired);              \
	}                                                                      \
        __always_inline                                                        \
	static inline bool prefix##_try_cmpxchg(                               \
		atomic_type *v, value_type *old, value_type desired)           \
	{                                                                      \
		return prefix##_try_cmpxchg_seq_cst(v, old, desired);          \
	}

#define ATOMIC_DEFINE_RMW_ORDER(prefix, atomic_type, value_type, suffix,       \
				order)                                         \
        __always_inline                                                        \
	static inline value_type prefix##_fetch_add_##suffix(                  \
		atomic_type *v, value_type value)                              \
	{                                                                      \
		return compiler_atomic_fetch_add_n(&v->counter, value, order); \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_add_fetch_##suffix(                  \
		atomic_type *v, value_type value)                              \
	{                                                                      \
		return compiler_atomic_add_fetch_n(&v->counter, value, order); \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_fetch_sub_##suffix(                  \
		atomic_type *v, value_type value)                              \
	{                                                                      \
		return compiler_atomic_fetch_sub_n(&v->counter, value, order); \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_sub_fetch_##suffix(                  \
		atomic_type *v, value_type value)                              \
	{                                                                      \
		return compiler_atomic_sub_fetch_n(&v->counter, value, order); \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_fetch_and_##suffix(                  \
		atomic_type *v, value_type value)                              \
	{                                                                      \
		return compiler_atomic_fetch_and_n(&v->counter, value, order); \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_and_fetch_##suffix(                  \
		atomic_type *v, value_type value)                              \
	{                                                                      \
		return compiler_atomic_and_fetch_n(&v->counter, value, order); \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_fetch_or_##suffix(                   \
		atomic_type *v, value_type value)                              \
	{                                                                      \
		return compiler_atomic_fetch_or_n(&v->counter, value, order);  \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_or_fetch_##suffix(                   \
		atomic_type *v, value_type value)                              \
	{                                                                      \
		return compiler_atomic_or_fetch_n(&v->counter, value, order);  \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_fetch_xor_##suffix(                  \
		atomic_type *v, value_type value)                              \
	{                                                                      \
		return compiler_atomic_fetch_xor_n(&v->counter, value, order); \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_xor_fetch_##suffix(                  \
		atomic_type *v, value_type value)                              \
	{                                                                      \
		return compiler_atomic_xor_fetch_n(&v->counter, value, order); \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_fetch_andnot_##suffix(               \
		atomic_type *v, value_type value)                              \
	{                                                                      \
		return compiler_atomic_fetch_and_n(&v->counter, ~value,        \
						   order);                     \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_andnot_fetch_##suffix(               \
		atomic_type *v, value_type value)                              \
	{                                                                      \
		return compiler_atomic_and_fetch_n(&v->counter, ~value,        \
						   order);                     \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_add_return_##suffix(                 \
		atomic_type *v, value_type value)                              \
	{                                                                      \
		return prefix##_add_fetch_##suffix(v, value);                  \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_sub_return_##suffix(                 \
		atomic_type *v, value_type value)                              \
	{                                                                      \
		return prefix##_sub_fetch_##suffix(v, value);                  \
	}                                                                      \
        __always_inline                                                        \
	static inline void prefix##_add_##suffix(atomic_type *v,               \
							  value_type value)    \
	{                                                                      \
		(void)prefix##_add_fetch_##suffix(v, value);                   \
	}                                                                      \
        __always_inline                                                        \
	static inline void prefix##_sub_##suffix(atomic_type *v,               \
							  value_type value)    \
	{                                                                      \
		(void)prefix##_sub_fetch_##suffix(v, value);                   \
	}                                                                      \
        __always_inline                                                        \
	static inline void prefix##_and_##suffix(atomic_type *v,               \
							  value_type value)    \
	{                                                                      \
		(void)prefix##_fetch_and_##suffix(v, value);                   \
	}                                                                      \
        __always_inline                                                        \
	static inline void prefix##_or_##suffix(atomic_type *v,                \
							 value_type value)     \
	{                                                                      \
		(void)prefix##_fetch_or_##suffix(v, value);                    \
	}                                                                      \
        __always_inline                                                        \
	static inline void prefix##_xor_##suffix(atomic_type *v,               \
							  value_type value)    \
	{                                                                      \
		(void)prefix##_fetch_xor_##suffix(v, value);                   \
	}                                                                      \
        __always_inline                                                        \
	static inline void prefix##_andnot_##suffix(atomic_type *v,            \
							     value_type value) \
	{                                                                      \
		(void)prefix##_fetch_andnot_##suffix(v, value);                \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_inc_return_##suffix(                 \
		atomic_type *v)                                                \
	{                                                                      \
		return prefix##_add_return_##suffix(v, 1);                     \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_dec_return_##suffix(                 \
		atomic_type *v)                                                \
	{                                                                      \
		return prefix##_sub_return_##suffix(v, 1);                     \
	}                                                                      \
        __always_inline                                                        \
	static inline void prefix##_inc_##suffix(atomic_type *v)               \
	{                                                                      \
		(void)prefix##_inc_return_##suffix(v);                         \
	}                                                                      \
        __always_inline                                                        \
	static inline void prefix##_dec_##suffix(atomic_type *v)               \
	{                                                                      \
		(void)prefix##_dec_return_##suffix(v);                         \
	}                                                                      \
        __always_inline                                                        \
	static inline bool prefix##_dec_and_test_##suffix(                     \
		atomic_type *v)                                                \
	{                                                                      \
		return prefix##_dec_return_##suffix(v) == 0;                   \
	}

#define ATOMIC_DEFINE_RMW_DEFAULT(prefix, atomic_type, value_type)             \
        __always_inline                                                        \
	static inline value_type prefix##_fetch_add(atomic_type *v,            \
							     value_type value) \
	{                                                                      \
		return prefix##_fetch_add_seq_cst(v, value);                   \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_add_fetch(atomic_type *v,            \
							     value_type value) \
	{                                                                      \
		return prefix##_add_fetch_seq_cst(v, value);                   \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_fetch_sub(atomic_type *v,            \
							     value_type value) \
	{                                                                      \
		return prefix##_fetch_sub_seq_cst(v, value);                   \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_sub_fetch(atomic_type *v,            \
							     value_type value) \
	{                                                                      \
		return prefix##_sub_fetch_seq_cst(v, value);                   \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_fetch_and(atomic_type *v,            \
							     value_type value) \
	{                                                                      \
		return prefix##_fetch_and_seq_cst(v, value);                   \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_and_fetch(atomic_type *v,            \
							     value_type value) \
	{                                                                      \
		return prefix##_and_fetch_seq_cst(v, value);                   \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_fetch_or(atomic_type *v,             \
							    value_type value)  \
	{                                                                      \
		return prefix##_fetch_or_seq_cst(v, value);                    \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_or_fetch(atomic_type *v,             \
							    value_type value)  \
	{                                                                      \
		return prefix##_or_fetch_seq_cst(v, value);                    \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_fetch_xor(atomic_type *v,            \
							     value_type value) \
	{                                                                      \
		return prefix##_fetch_xor_seq_cst(v, value);                   \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_xor_fetch(atomic_type *v,            \
							     value_type value) \
	{                                                                      \
		return prefix##_xor_fetch_seq_cst(v, value);                   \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_fetch_andnot(                        \
		atomic_type *v, value_type value)                              \
	{                                                                      \
		return prefix##_fetch_andnot_seq_cst(v, value);                \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_andnot_fetch(                        \
		atomic_type *v, value_type value)                              \
	{                                                                      \
		return prefix##_andnot_fetch_seq_cst(v, value);                \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_add_return(                          \
		atomic_type *v, value_type value)                              \
	{                                                                      \
		return prefix##_add_return_seq_cst(v, value);                  \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_sub_return(                          \
		atomic_type *v, value_type value)                              \
	{                                                                      \
		return prefix##_sub_return_seq_cst(v, value);                  \
	}                                                                      \
        __always_inline                                                        \
	static inline void prefix##_add(atomic_type *v,                        \
						 value_type value)             \
	{                                                                      \
		prefix##_add_seq_cst(v, value);                                \
	}                                                                      \
        __always_inline                                                        \
	static inline void prefix##_sub(atomic_type *v,                        \
						 value_type value)             \
	{                                                                      \
		prefix##_sub_seq_cst(v, value);                                \
	}                                                                      \
        __always_inline                                                        \
	static inline void prefix##_and(atomic_type *v,                        \
						 value_type value)             \
	{                                                                      \
		prefix##_and_seq_cst(v, value);                                \
	}                                                                      \
        __always_inline                                                        \
	static inline void prefix##_or(atomic_type *v,                         \
						value_type value)              \
	{                                                                      \
		prefix##_or_seq_cst(v, value);                                 \
	}                                                                      \
        __always_inline                                                        \
	static inline void prefix##_xor(atomic_type *v,                        \
						 value_type value)             \
	{                                                                      \
		prefix##_xor_seq_cst(v, value);                                \
	}                                                                      \
        __always_inline                                                        \
	static inline void prefix##_andnot(atomic_type *v,                     \
						    value_type value)          \
	{                                                                      \
		prefix##_andnot_seq_cst(v, value);                             \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_inc_return(atomic_type *v)           \
	{                                                                      \
		return prefix##_inc_return_seq_cst(v);                         \
	}                                                                      \
        __always_inline                                                        \
	static inline value_type prefix##_dec_return(atomic_type *v)           \
	{                                                                      \
		return prefix##_dec_return_seq_cst(v);                         \
	}                                                                      \
        __always_inline                                                        \
	static inline void prefix##_inc(atomic_type *v)                        \
	{                                                                      \
		prefix##_inc_seq_cst(v);                                       \
	}                                                                      \
        __always_inline                                                        \
	static inline void prefix##_dec(atomic_type *v)                        \
	{                                                                      \
		prefix##_dec_seq_cst(v);                                       \
	}                                                                      \
        __always_inline                                                        \
	static inline bool prefix##_dec_and_test(atomic_type *v)               \
	{                                                                      \
		return prefix##_dec_and_test_seq_cst(v);                       \
	}

#define ATOMIC_DEFINE_API(prefix, atomic_type, value_type)                     \
	ATOMIC_DEFINE_INIT_API(prefix, atomic_type, value_type)                \
	ATOMIC_DEFINE_ACCESS_API(prefix, atomic_type, value_type)              \
	ATOMIC_DEFINE_CMPXCHG_API(prefix, atomic_type, value_type)             \
	ATOMIC_DEFINE_RMW_ORDER(prefix, atomic_type, value_type, relaxed,      \
				ATOMIC_ORDER_RELAXED)                          \
	ATOMIC_DEFINE_RMW_ORDER(prefix, atomic_type, value_type, acquire,      \
				ATOMIC_ORDER_ACQUIRE)                          \
	ATOMIC_DEFINE_RMW_ORDER(prefix, atomic_type, value_type, release,      \
				ATOMIC_ORDER_RELEASE)                          \
	ATOMIC_DEFINE_RMW_ORDER(prefix, atomic_type, value_type, acq_rel,      \
				ATOMIC_ORDER_ACQ_REL)                          \
	ATOMIC_DEFINE_RMW_ORDER(prefix, atomic_type, value_type, seq_cst,      \
				ATOMIC_ORDER_SEQ_CST)                          \
	ATOMIC_DEFINE_RMW_DEFAULT(prefix, atomic_type, value_type)

ATOMIC_DEFINE_API(atomic, atomic_t, int32_t)
ATOMIC_DEFINE_API(atomic64, atomic64_t, int64_t)
ATOMIC_DEFINE_API(atomic_isize, atomic_isize_t, isize)

#undef ATOMIC_DEFINE_API
#undef ATOMIC_DEFINE_RMW_DEFAULT
#undef ATOMIC_DEFINE_RMW_ORDER
#undef ATOMIC_DEFINE_CMPXCHG_API
#undef ATOMIC_DEFINE_CMPXCHG_ORDER
#undef ATOMIC_DEFINE_ACCESS_API
#undef ATOMIC_DEFINE_INIT_API

#endif
