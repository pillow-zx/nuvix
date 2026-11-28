#ifndef _NUVIX_MACRO_H
#define _NUVIX_MACRO_H

#define STRLEN(str)            (sizeof(str) - 1)
#define ARRLEN(arr)            ((int)(sizeof(arr) / sizeof(arr[0])))

/*
 * ============================================================================
 *  IFDEF / IFNDEF / IFONE / IFZERO
 * ============================================================================
 *
 * These macros implement a compile-time "if" over the value/definition of
 * another macro. They all eventually expand to either
 *
 *      __KEEP(__VA_ARGS__)    -> __VA_ARGS__   (keep the code)
 *      __IGNORE(__VA_ARGS__)  -> nothing       (drop the code)
 *
 * so the selected branch is the only one that survives preprocessing.
 *
 * Quick summary:
 *
 *      IFDEF(M, ...)   true iff M is defined as 0 or 1
 *      IFNDEF(M, ...)  true iff M is NOT defined as 0 or 1
 *      IFONE(M, ...)   true iff M expands to 1
 *      IFZERO(M, ...)  true iff M expands to 0
 *
 * Note: this particular implement only recognises the replacement
 * tokens 0 and 1 (see __P_DEF_0/__P_DEF_1 below). A macro defined to an
 * arbitrary token, or to nothing, is not detected by IFDEF here.
 *
 * ----------------------------------------------------------------------------
 *  1. The comma trick
 * ----------------------------------------------------------------------------
 *
 * The decision is encoded as "does the first argument contain an comma?".
 *
 *      __MUX_WITH_COMMA(contain_comma, A, B)
 *          -> __SECOND(contain_comma A, B)
 *
 * If contain_comma ends with a comma:
 *
 *      __SECOND(X, A, B)          // three args, __SECOND picks A
 *
 *  If contain_comma has no comma:
 *
 *      __SECOND(X A, B)           // two args, __SECOND picks B
 *
 *  Diagram:
 *
 *      contain_comma = "X,"        contain_comma = "X"
 *            |                           |
 *            v                           v
 *      __SECOND(X, A, B)           __SECOND(X A, B)
 *            |                           |
 *            v                           v
 *            A                           B
 *
 *  __SECOND(a, b, ...) is defined as b, i.e. "return the second argument".
 *
 * ----------------------------------------------------------------------------
 *  2. How a definition becomes a comma
 * ----------------------------------------------------------------------------
 *
 *      __MUX_MACRO_PROPERTY(p, macro, a, b)
 *          -> __MUX_WITH_COMMA(CONCAT(p, macro), a, b)
 *
 *  CONCAT(p, macro) pastes p and the *expanded* value of macro.  For example:
 *
 *      #define FOO 1
 *      CONCAT(__P_DEF_, FOO)  ->  __P_DEF_1  ->  X,
 *
 *  because:
 *
 *      #define __P_DEF_1  X,
 *      #define __P_DEF_0  X,
 *
 *  Both 0 and 1 are mapped to "X," so IFDEF treats a macro defined as either
 *  0 or 1 as "defined".  If the macro is undefined, the paste produces an
 *  undefined identifier such as __P_DEF_BAR, which expands to nothing, so
 *  there is no comma and the false branch is chosen.
 *
 *  IFNDEF simply swaps the two branches:
 *
 *      __MUXNDEF(macro, x, y) -> __MUX_MACRO_PROPERTY(__P_DEF_, macro, y, x)
 *
 *  so "defined" selects y (usually __IGNORE) and "undefined" selects x.
 *
 *  IFONE / IFZERO use the same mechanism with different tables:
 *
 *      #define __P_ONE_1   X,     // only 1 matches
 *      #define __P_ZERO_0  X,     // only 0 matches
 *
 *  Therefore:
 *
 *      IFONE(FOO, ...)  -> true only when FOO expands to 1
 *      IFZERO(FOO, ...) -> true only when FOO expands to 0
 *
 * ----------------------------------------------------------------------------
 *  3. Expansion traces
 * ----------------------------------------------------------------------------
 *
 *  Case A: #define FOO 1
 *
 *      IFDEF(FOO, KEEP_ME)
 *        -> __MUXDEF(FOO, __KEEP, __IGNORE)(KEEP_ME)
 *        -> __MUX_MACRO_PROPERTY(__P_DEF_, 1, __KEEP, __IGNORE)(KEEP_ME)
 *        -> __MUX_WITH_COMMA(CONCAT(__P_DEF_, 1), __KEEP, __IGNORE)(KEEP_ME)
 *        -> __MUX_WITH_COMMA(__P_DEF_1, __KEEP, __IGNORE)(KEEP_ME)
 *        -> __MUX_WITH_COMMA(X,, __KEEP, __IGNORE)(KEEP_ME)
 *        -> __SECOND(X, __KEEP, __IGNORE)(KEEP_ME)
 *        -> __KEEP(KEEP_ME)
 *        -> KEEP_ME
 *
 *  Case B: FOO is undefined
 *
 *      IFDEF(FOO, KEEP_ME)
 *        -> __MUXDEF(FOO, __KEEP, __IGNORE)(KEEP_ME)
 *        -> __MUX_MACRO_PROPERTY(__P_DEF_, FOO, __KEEP, __IGNORE)(KEEP_ME)
 *        -> __MUX_WITH_COMMA(__P_DEF_FOO, __KEEP, __IGNORE)(KEEP_ME)
 *        -> __SECOND(__P_DEF_FOO __KEEP, __IGNORE)(KEEP_ME)
 *        -> __IGNORE(KEEP_ME)
 *        -> (nothing)
 *
 *  For IFNDEF the same decision is made, but the branches are swapped, so
 *  Case A yields nothing and Case B yields KEEP_ME.
 *
 * ----------------------------------------------------------------------------
 *  4. Why the extra CONCAT layer?  (macro expansion / recursion)
 * ----------------------------------------------------------------------------
 *
 *      #define _CONCAT(x, y)  x##y
 *      #define CONCAT(x, y)   _CONCAT(x, y)
 *
 *  The ## operator pastes tokens *without* expanding its operands first.
 *  If we wrote _CONCAT(__P_DEF_, FOO) directly, we would get the single
 *  identifier __P_DEF_FOO, and the fact that FOO is defined as 1 would be
 *  lost.  The outer CONCAT is an ordinary macro: its arguments are expanded
 *  before being passed to _CONCAT, so FOO first becomes 1, and only then is
 *  __P_DEF_ pasted with 1.
 *
 *  The whole chain is also a lesson in macro rescanning.  C preprocessor
 *  macros cannot truly recurse: while a macro is being expanded, its own
 *  name is "painted blue" and is not expanded again inside that expansion.
 *  These macros avoid self-recursion by using different helper names for
 *  each stage (__MUXDEF -> __MUX_MACRO_PROPERTY -> __MUX_WITH_COMMA ->
 *  __SECOND).  Each stage expands its arguments and hands the result to the
 *  next stage.  __KEEP and __IGNORE are function-like macros but are passed
 *  around *without* parentheses, so they stay as plain tokens until the
 *  final "( __VA_ARGS__ )" invokes the selected one.
 *
 * ----------------------------------------------------------------------------
 *  5. Example
 * ----------------------------------------------------------------------------
 *
 *      #define FEATURE 1
 *
 *      IFDEF(FEATURE,  int x = 1;)   // -> int x = 1;
 *      IFNDEF(FEATURE, int x = 1;)   // -> (nothing)
 *      IFONE(FEATURE,  int y = 2;)   // -> int y = 2;
 *      IFZERO(FEATURE, int y = 2;)   // -> (nothing)
 *
 *      #define MODE 0
 *
 *      IFDEF(MODE,  int a = 1;)      // -> int a = 1;   (0 counts as defined)
 *      IFONE(MODE,  int b = 2;)      // -> (nothing)
 */
#define _CONCAT(x, y)          x##y
#define CONCAT(x, y)           _CONCAT(x, y)
#define CONCAT3(x, y, z)       CONCAT(CONCAT(x, y), z)
#define CONCAT4(x, y, z, w)    CONCAT3(CONCAT(x, y), z, w)
#define CONCAT5(x, y, z, w, v) CONCAT4(CONCAT(x, y), z, w, v)

#define __SECOND(a, b, ...)    b
#define __THIRD(a, b, c, ...)  c
#define __MUX_WITH_COMMA(contain_comma, a, b) __SECOND(contain_comma a, b)
#define __MUX_MACRO_PROPERTY(p, macro, a, b)                                   \
    __MUX_WITH_COMMA(CONCAT(p, macro), a, b)

#define __P_DEF_0              X,
#define __P_DEF_1              X,
#define __P_ONE_1              X,
#define __P_ZERO_0             X,
#define __MUXDEF(macro, x, y)  __MUX_MACRO_PROPERTY(__P_DEF_, macro, x, y)
#define __MUXNDEF(macro, x, y) __MUX_MACRO_PROPERTY(__P_DEF_, macro, y, x)
#define __MUXONE(macro, x, y)  __MUX_MACRO_PROPERTY(__P_ONE_, macro, x, y)
#define __MUXZERO(macro, x, y) __MUX_MACRO_PROPERTY(__P_ZERO_, macro, x, y)

#define __IGNORE(...)
#define __KEEP(...)        __VA_ARGS__
#define IFDEF(macro, ...)  __MUXDEF(macro, __KEEP, __IGNORE)(__VA_ARGS__)
#define IFNDEF(macro, ...) __MUXNDEF(macro, __KEEP, __IGNORE)(__VA_ARGS__)
#define IFONE(macro, ...)  __MUXONE(macro, __KEEP, __IGNORE)(__VA_ARGS__)
#define IFZERO(macro, ...) __MUXZERO(macro, __KEEP, __IGNORE)(__VA_ARGS__)

#define TYPEOF(x)          __typeof__(x)
#define ALIGNOF(x)         __alignof__(x)
#define UNIQUE(prefix)     CONCAT(prefix_, __COUNTER__)

#define READ_ONCE(x)       (*(volatile TYPEOF(x) *)&(x))
#define WRITE_ONCE(x, val) (*(volatile TYPEOF(x) *)&(x) = (val))

#define typecheck(type, expr)                                                  \
    ({                                                                         \
        type __dummy;                                                          \
        TYPEOF(expr) __dummy2;                                                 \
        (void)(&__dummy == &__dummy2);                                         \
        1;                                                                     \
    })

/*
 * container_of - given the address of a structure member, recover the
 *                address of the  enclosing structure.
 *
 * @ptr     : pointer to the structure member; must be a  real member address
 *            such as &obj.member
 * @type    : type of the structure that contains the member
 * @member  : name of the member within the structure
 * Returns  : pointer to the start of the enclosing structure, typed as type *
 *
 * Use case : recover the host structure when only a member pointer is held.
 * Typical example:
 *       struct task *t = container_of(pos, struct task, thread);
 *
 * Principle: struct base address = member addr - byte offset of member
 *
 *  low address                                     high address
 *    │                                                 │
 *    ▼                                                 ▼
 *    ┌──────────┬─────────┬──────────────┬─────────────┐
 *    │ member x │   ...   │    member    │  member y   │
 *    └──────────┴─────────┴──────────────┴─────────────┘
 *    ▲                    ▲
 *    │                    │
 *  struct base          ptr (passed-in member pointer) = (char *)ptr - offsetof(type, member)
 *
 * Step by step:
 *   1) ((type *)0)->member yields the member type and offset at compile time;
 *      it is never actually dereferenced. TYPEOF obtains the member type,
 *      and _p is declared with that type and initialized from ptr.
 *      if ptr is incompatible with the member type, this triggers a
 *      compile-time warning (type checking).
 *   2) Cast to char * before subtracting the offset: pointer arithmetic
 *      scales by the pointed-to type, so only char * steps by exact bytes.
 *      Using the member type pointer would scale by that type's size.
 *   3) The result is the structure base address; cast it back to type *
 *      and return it.
 *
 * Notes:
 *    - ptr must be a real member address; passing NULL or a valid pointer
 *      will not be diagnosed, but the result is meaningless.
 *    - member must not be a bit-field (offsetof is invalid for bit-fields).
 */
#define container_of(ptr, type, member)                                        \
    ({                                                                         \
        const TYPEOF(((type *)0)->member) *_p = (ptr);                         \
        (type *)((char *)_p - offsetof(type, member));                         \
    })

#define member_size(type, member) sizeof(((type *)0)->member)

/*
 * ============================================================================
 *  ERR_PTR / PTR_ERR / IS_ERR  --  encoding error codes as pointers
 * ============================================================================
 *
 * A function that normally returns a pointer has a problem: how does it
 * report failure? It cannot return NULL for every error, because NULL is
 * a prefectly valid "no object" answer, and the caller also wants to know
 * "which" error happened (ENOMEM ? EINVAL ? ENOENT?).
 *
 * The trick used here: smuggle a negative errno into the pointer itself.
 * A small negative integer, reinterpreted as an address, land in the very
 * top of the address space -- a region that can never be a real object.
 * So on pointer-sized return value carries both "success with an address"
 * and "failure with an errno"
 *
 * ----------------------------------------------------------------------------
 *  The encoding
 * ----------------------------------------------------------------------------
 *
 *  Valid error pointers occupy the top MAX_ERRNO (4095) bytes of the address
 *  space, i.e. the range of values -4095 .. -1 interpreted as a signed
 *  integer, or equivalently 0xFFFFF001 .. 0xFFFFFFFF as unsigned:
 *
 *      32-bit:   0xFFFFF001 .. 0xFFFFFFFF
 *      64-bit:   0xFFFFFFFFFFFFF001 .. 0xFFFFFFFFFFFFFFFF
 *
 *  Everything below 0xFFFFF001 (32-bit) / 0xFFFFFFFFFFFFF001 (64-bit) is
 *  treated as a legitimate pointer, including NULL:
 *
 *      ... real kernel objects ... | NULL | ... | err pointers |
 *      ^                                ^                      ^
 *      0                          0x...F001             0x...FFFF
 *                                                    (-MAX_ERRNO .. -1)
 *
 *  MAX_ERRNO = 4095 is the largest errno Linux ever produces(nuvix use tha same),
 *  so 4095 slots are enough to hold every possible error. 0 is not an error and is not
 *  encodable here -- NULL stays NULL.
 */

 /* Largest errno value; bounds the size of the reserved top region. */
#define MAX_ERRNO                 4095

/*
 * IS_ERR_VALUE - is this raw integer an encoded error?
 *
 * The cast to unsigned long is what makes this well-defined and branch-free:
 * instead of comparing signed negatives (and worrying about overflow of
 * -MAX_ERRNO in int), we compare in unsigned arithmetic, where the top
 *  region becomes the largest values. Only the top 4095 values pass, so a
 *  normal pointer -- even NULL, even a huge but valid kernel address -- never
 *  matches. Note it is ">=", so -1 .. -4095 all count as errors.
 */
#define IS_ERR_VALUE(x)   ((unsigned long)(x) >= (unsigned long)(-MAX_ERRNO))


/*
 * ERR_PTR - trun a negative errno into a pointer-shaped error value.
 *
 * @err : a *negative* errno, e.g. -ENOMEM, -EINVAL (never the positive form!)
 *
 * The int is cast to long first, so the sign is extended across the whole
 * word: -ENOMEM (int -12) becomes 0xFFFFFFFFFFFFFFF4 on 64-bit, not
 * 0x00000000FFFFFFF4. Without that sign extension the value would land in
 * the middle of the address space and IS_ERR would reject it.
 *
 * Passing a positive number here is a bug: ERRR_PTR(ENOMEM) yields the address
 * 0x0000...000C, which is not recognised as an error by IS_ERR.
 */
#define ERR_PTR(err)      ((void *)(long)(err))

/*
 * PTR_ERR - recover the errno from an error pointer.
 *
 * @ptr : a pointer for which IS_ERR(ptr) is true
 *
 * Cast back through long to int, truncating to 32 bits and preserving the
 * sign, so 0xFFFFFFFFFFFFFFF4 becomes -ENOMEM again. The result is the
 * usual negative errno convention: callers do "return PTR_ERR(p);" to
 * propagate the failure.
 *
 * Only valid on real error pointer. Calling it on a normal address just
 * truncates the address to garbage.
 */
#define PTR_ERR(ptr)      ((int)(long)(ptr))

/*
 * IS_ERR - does this pointer actually carrry an encoded errorr?
 *
 * Convenience wrapper around IS_ERR_VALUE; the double cast to unsigned long
 * (here and inside IS_ERR_VALUE) is redundant but harmless. Callers must
 * test this *before* dereferencing, and before PTR_ERR is meaningful:
 *
 *      struct foo *f = get_foo();
 *      if (IS_ERR(f))
 *              return PTR_ERR(f);      // propagate -ENOMEM etc.
 *      use(f);
 *
 */
#define IS_ERR(ptr)       IS_ERR_VALUE((unsigned long)(ptr))

#define auto              __auto_type

#define IN_RANGE(n, s, e) (((n) >= (s)) && ((n) <= (e)))

#endif /* ifndef _NUVIX_MACRO_H */
