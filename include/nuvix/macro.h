#ifndef _NUVIX_MACRO_H
#define _NUVIX_MACRO_H

#define STRLEN(str)            (sizeof(str) - 1)
#define ARRLEN(arr)            ((int)(sizeof(arr) / sizeof(arr[0])))

#define _CONCAT(x, y)          x##y
#define CONCAT(x, y)           _CONCAT(x, y)
#define CONCAT3(x, y, z)       CONCAT(CONCAT(x, y), z)
#define CONCAT4(x, y, z, w)    CONCAT3(CONCAT(x, y), z, w)
#define CONCAT5(x, y, z, w, v) CONCAT4(CONCAT(x, y), z, w, v)

#define __SECOND(a, b, ...)    b
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

#define container_of(ptr, type, member)                                        \
    ({                                                                         \
        const TYPEOF(((type *)0)->member) *_p = (ptr);                         \
        (type *)((char *)_p - offsetof(type, member));                         \
    })

#define member_size(type, member) sizeof(((type *)0)->member)

#define MAX_ERRNO                 4095
#define IS_ERR_VALUE(x) ((unsigned long)(x) >= (unsigned long)(-MAX_ERRNO))
#define ERR_PTR(err)    ((void *)(long)(err))
#define PTR_ERR(ptr)    ((int)(long)(ptr))
#define IS_ERR(ptr)     IS_ERR_VALUE((unsigned long)(ptr))

#endif /* ifndef _NUVIX_MACRO_H */
