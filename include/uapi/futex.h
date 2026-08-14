#ifndef _NUVIX_UAPI_FUTEX_H
#define _NUVIX_UAPI_FUTEX_H

/**
 * @file futex.h
 * @brief Linux futex command bits and robust-list UAPI layouts.
 */

#define FUTEX_WAIT	      0
#define FUTEX_WAKE	      1
#define FUTEX_FD	      2
#define FUTEX_REQUEUE	      3
#define FUTEX_CMP_REQUEUE     4
#define FUTEX_WAKE_OP	      5
#define FUTEX_LOCK_PI	      6
#define FUTEX_UNLOCK_PI	      7
#define FUTEX_TRYLOCK_PI      8
#define FUTEX_WAIT_BITSET     9
#define FUTEX_WAKE_BITSET     10
#define FUTEX_WAIT_REQUEUE_PI 11
#define FUTEX_CMP_REQUEUE_PI  12
#define FUTEX_LOCK_PI2	      13

#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_CMD_MASK	     (~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME))

#define FUTEX_WAIT_PRIVATE	  (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE	  (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)
#define FUTEX_REQUEUE_PRIVATE	  (FUTEX_REQUEUE | FUTEX_PRIVATE_FLAG)
#define FUTEX_CMP_REQUEUE_PRIVATE (FUTEX_CMP_REQUEUE | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_OP_PRIVATE	  (FUTEX_WAKE_OP | FUTEX_PRIVATE_FLAG)
#define FUTEX_LOCK_PI_PRIVATE	  (FUTEX_LOCK_PI | FUTEX_PRIVATE_FLAG)
#define FUTEX_LOCK_PI2_PRIVATE	  (FUTEX_LOCK_PI2 | FUTEX_PRIVATE_FLAG)
#define FUTEX_UNLOCK_PI_PRIVATE	  (FUTEX_UNLOCK_PI | FUTEX_PRIVATE_FLAG)
#define FUTEX_TRYLOCK_PI_PRIVATE  (FUTEX_TRYLOCK_PI | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAIT_BITSET_PRIVATE (FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_BITSET_PRIVATE (FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAIT_REQUEUE_PI_PRIVATE                                          \
	(FUTEX_WAIT_REQUEUE_PI | FUTEX_PRIVATE_FLAG)
#define FUTEX_CMP_REQUEUE_PI_PRIVATE (FUTEX_CMP_REQUEUE_PI | FUTEX_PRIVATE_FLAG)

#define FUTEX_WAITERS	       0x80000000U
#define FUTEX_OWNER_DIED       0x40000000U
#define FUTEX_TID_MASK	       0x3fffffffU
#define FUTEX_BITSET_MATCH_ANY 0xffffffffU

#define FUTEX_OP_SET	     0
#define FUTEX_OP_ADD	     1
#define FUTEX_OP_OR	     2
#define FUTEX_OP_ANDN	     3
#define FUTEX_OP_XOR	     4
#define FUTEX_OP_OPARG_SHIFT 8

#define FUTEX_OP_CMP_EQ 0
#define FUTEX_OP_CMP_NE 1
#define FUTEX_OP_CMP_LT 2
#define FUTEX_OP_CMP_LE 3
#define FUTEX_OP_CMP_GT 4
#define FUTEX_OP_CMP_GE 5

#define FUTEX_OP(op, oparg, cmp, cmparg)                                       \
	(((op & 0xf) << 28) | ((cmp & 0xf) << 24) | ((oparg & 0xfff) << 12) |  \
	 (cmparg & 0xfff))

/**
 * @struct robust_list
 * @brief Intrusive userspace node used by Linux robust futex lists.
 *
 * @par Fields
 * - @c next: Next userspace robust-list node.
 */
struct robust_list {
	struct robust_list *next;
};

/**
 * @struct robust_list_head
 * @brief Userspace robust futex list head registered per task.
 *
 * @par Fields
 * - @c list: Circular list head.
 * - @c futex_offset: Offset from node address to futex word.
 * - @c list_op_pending: Node being modified.
 */
struct robust_list_head {
	struct robust_list list;
	long futex_offset;
	struct robust_list *list_op_pending;
};

#undef offsetof
#define offsetof(t, d) __builtin_offsetof(t, d)

_Static_assert(sizeof(struct robust_list_head) == 24,
	       "robust_list_head ABI size mismatch");
_Static_assert(offsetof(struct robust_list_head, list_op_pending) == 16,
	       "robust_list_head list_op_pending ABI offset mismatch");

#endif
