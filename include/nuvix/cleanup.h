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

#endif
