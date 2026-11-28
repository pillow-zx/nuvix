#ifndef _NUVIX_CLEANUP_H
#define _NUVIX_CLEANUP_H

#include <nuvix/ctypes.h>
#include <nuvix/macro.h>
#include <nuvix/compiler.h>

/*
 * ============================================================================
 *  Scope-based cleanup helpers
 * ============================================================================
 *
 *  GCC/Clang provide __attribute__((cleanup(fn))): a local variable carrying
 *  that attribute is automatically passed to fn(&var) when it goes out of
 *  scope -- by normal fall-through, by return, by break/continue, or by a
 *  goto out of the block.  fn must have the shape:
 *
 *      void fn(void *p);        // p actually points to the *variable*
 *
 *  and it is invoked with the variable's address, not its value, so that it
 *  can read (and, if it wants, rewrite) the variable before it dies:
 *
 *      void release(void *p) { free(*(void **)p); }
 *      void f(void) {
 *          __attribute__((cleanup(release))) void *buf = malloc(64);
 *          ...
 *      }                        // release(&buf) runs here -> free(buf)
 *
 *  These macros build a small, typed wrapper on top of that attribute so the
 *  caller writes clean, explicit ownership code:
 *
 *      DECLARE_FREE(file, FILE *, fclose(_T));
 *
 *      void work(void) {
 *          __free(file) FILE *f = fopen(...);   // auto-fclosed
 *          ...
 *          return move(f);                     // owner transferred
 *      }
 *
 *  The mental model is a per-variable "armed/disarmed" cleanup: the variable
 *  owns a resource while it holds a non-null value; setting it to the type's
 *  null value ("invalid") disarms the cleanup, so the destructor becomes a
 *  no-op.  move() does exactly that disarming and returns the old value; it
 *  is how ownership is handed off without double frees.
 *
 * ----------------------------------------------------------------------------
 *  __cleanup(func)
 * ----------------------------------------------------------------------------
 *
 *  Thin, portable wrapper around the compiler's cleanup attribute.  Only
 *  defined when the attribute is actually available (__has_attribute), so
 *  the rest of the header degrades to "no cleanup" rather than a hard error
 *  on compilers that do not support it.
 */
#if __has_attribute(cleanup)
#define __cleanup(func) __attribute__((__cleanup__(func)))
#endif

/*
 * DECLARE_FREE(_name, _type, _free)
 *
 * Define a cleanup function __cleanup_<name> for a resource of type _type,
 * with _free giving the release code.  Example:
 *
 *      DECLARE_FREE(fd, int, close(_T));
 *
 * expands to (roughly):
 *
 *      static __maybe_unused __always_inline void __cleanup_fd(void *p)
 *      {
 *          int _T = *(int *)p;    // copy the variable's current value
 *          close(_T);             // run the user-supplied release code
 *      }
 *
 * Notes:
 *   - p is the address of the *variable*, hence the dereference through
 *     (_type *)p to recover its value.  The value is copied into _T so the
 *     _free expression can refer to it as _T (readable, no aliasing games).
 *   - _free is a statement/expression fragment, not a function name, e.g.
 *     "fclose(_T)" or "free(_T)".
 *   - __maybe_unused keeps the compiler quiet if a translation unit declares
 *     the cleanup but never uses __free(name); __always_inline removes the
 *     call overhead and lets the compiler see through the indirection.
 *   - The null value of _type must be safe to pass to _free, because a
 *     disarmed variable (set to null by move/keep) will still be handed to
 *     this function at scope exit.
 */
#define DECLARE_FREE(_name, _type, _free)                                      \
    static __maybe_unused __always_inline void __cleanup_##_name(void *p)      \
    {                                                                          \
        _type _T = *(_type *)p;                                                \
        _free;                                                                 \
    }

/*
 * __free(name)
 *
 * Attach the cleanup declared by DECLARE_FREE(name, ...) to a variable:
 *
 *      int f __free(fd) = open(...);      // auto-closed at scope exit
 *
 * Expands to __cleanup(__cleanup_<name>), i.e. the GCC attribute referring
 * to the generated destructor.
 */
#define __free(name) __cleanup(__cleanup_##name)

/*
 * __move(p, invalid)
 *
 * Move-out primitive: read *p, overwrite *p with `invalid`, return the old
 * value.  In one expression it (a) transfers the resource to the caller and
 * (b) disarms the cleanup on p, because p now holds the null value:
 *
 *      __auto_type __pp  = &(p);     // address of the variable
 *      __auto_type __tmp = *__pp;    // save the resource
 *      *__pp = (invalid);            // disarm: cleanup sees null
 *      __tmp;                        // the statement expression's value
 *
 * Taking the address and writing through it is deliberate: it guarantees p
 * is a modifiable lvalue, and the trailing __tmp makes the block evaluate
 * to the original value (GCC statement expressions yield the value of the
 * last expression).
 *
 *   before:  p = R          after:  p = invalid,  result = R
 *
 * The moved value R is now owned by the caller; when p leaves scope the
 * cleanup fires on `invalid` and does nothing useful (typically a no-op).
 */
#define __move(p, invalid)                                                     \
    ({                                                                         \
        __auto_type __pp = &(p);                                               \
        __auto_type __tmp = *__pp;                                             \
        *__pp = (invalid);                                                     \
        __tmp;                                                                 \
    })

/*
 * __move_1(p)          -- move with the type's default null value
 * __move_2(p, invalid) -- move with an explicit null value
 *
 * __move_1 synthesizes the zero value with a C99 compound literal,
 * (TYPEOF(p)){0}, so it works for scalars and small structs alike without
 * the caller having to spell out the type.  __move_2 is for types whose
 * "empty" value is not all-zero (e.g. a handle sentinel of -1):
 *
 *      __move_1(f)              // f = (FILE *)0
 *      __move_2(h, (handle)-1)  // h = (handle)-1
 *
 * (TYPEOF(p)){0} requires p to name a variable/expression whose type is
 * known; prefer passing a plain variable to these macros.
 */
#define __move_1(p)          __move(p, (TYPEOF(p)){0})
#define __move_2(p, invalid) __move(p, invalid)

/*
 * move(...)  -- dispatch to __move_1 or __move_2 by argument count
 *
 *      move(p)          -> __move_1(p)          (null = (TYPEOF(p)){0})
 *      move(p, invalid) -> __move_2(p, invalid)
 *
 * The dispatch is the classic "pick the Nth argument" trick:
 *
 *      __THIRD(a, b, c, ...)   returns c
 *
 *   one arg, p:
 *      __THIRD(p,               __move_2, __move_1)  -> __move_1
 *              ^               ^         ^
 *              a               b         c = result
 *
 *   two args, p, inv:
 *      __THIRD(p,  inv,         __move_2, __move_1)  -> __move_2
 *              ^   ^            ^
 *              a   b            c = result
 *
 * So the trailing names shift into position c only when the argument count
 * matches; this is why move() supports an optional second parameter.
 *
 * Usage:
 *      return move(f);              // hand the resource to the caller
 *      g = move(f);                 // take ownership locally
 */
#define move(...) __THIRD(__VA_ARGS__, __move_2, __move_1)(__VA_ARGS__)

/*
 * keep(p)
 *
 * Explicitly give up ownership *without* transferring it: p is reset to its
 * null value and the old value is discarded, so the scope-exit cleanup sees
 * null and does nothing.  Use it to document "this resource is intentionally
 * leaked / owned elsewhere / not my problem anymore":
 *
 *      __free(fd) int f = open(...);
 *      if (handed_to_other_subsystem)
 *              keep(f);             // let it live beyond this scope
 *
 * The (void) cast silences "value computed but not used" and makes the
 * intent clear: the point is the side effect on p, not the result.
 */
#define keep(p)   ((void)move(p))

#endif
