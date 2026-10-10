#ifndef _NUVIX_CLEANUP_H
#define _NUVIX_CLEANUP_H

#include <nuvix/ctypes.h>
#include <nuvix/macro.h>
#include <nuvix/compiler.h>

#if __has_attribute(cleanup)
#define __cleanup(func) __attribute__((__cleanup__(func)))
#endif

#define DECLARE_FREE(_name, _type, _free)                                      \
    static __maybe_unused __always_inline void __cleanup_##_name(void *p)      \
    {                                                                          \
        _type _T = *(_type *)p;                                                \
        _free;                                                                 \
    }

#define __free(name) __cleanup(__cleanup_##name)

#define __move(p, invalid)                                                     \
    ({                                                                         \
        __auto_type __pp = &(p);                                               \
        __auto_type __tmp = *__pp;                                             \
        *__pp = (invalid);                                                     \
        __tmp;                                                                 \
    })

#define __move_1(p)          __move(p, (TYPEOF(p)){0})
#define __move_2(p, invalid) __move(p, invalid)
#define move(...) __THIRD(__VA_ARGS__, __move_2, __move_1)(__VA_ARGS__)

#define keep(p)   ((void)move(p))

#define DECLARE_GUARD(_name, _type, _acquire, _release, ...)                   \
    typedef struct {                                                           \
        _type value;                                                           \
        __VA_ARGS__;                                                           \
    } guard_##_name##_t;                                                       \
    static __maybe_unused __always_inline void __guard_##_name##_leave(        \
        guard_##_name##_t *state)                                              \
    {                                                                          \
        _release;                                                              \
    }                                                                          \
    static __maybe_unused __always_inline guard_##_name##_t                    \
    __guard_##_name##_enter(_type value)                                       \
    {                                                                          \
        guard_##_name##_t saved = {.value = value};                            \
        guard_##_name##_t *state = &saved;                                     \
        _acquire;                                                              \
        return saved;                                                          \
    }

#define guard(name, value)                                                     \
    guard_##name##_t UNIQUE(guard) __cleanup(__guard_##name##_leave) =         \
        __guard_##name##_enter(value)

#endif
