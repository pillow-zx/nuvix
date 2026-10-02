#ifndef _NUVIX_MATH_H
#define _NUVIX_MATH_H

#include <nuvix/assert.h>
#include <nuvix/compiler.h>

#define MIN(a, b)                                                              \
    ({                                                                         \
        __auto_type _a = (a);                                                  \
        __auto_type _b = (b);                                                  \
        _a < _b ? _a : _b;                                                     \
    })

#define MAX(a, b)                                                              \
    ({                                                                         \
        __auto_type _a = (a);                                                  \
        __auto_type _b = (b);                                                  \
        _a > _b ? _a : _b;                                                     \
    })

#define CLAMP(x, lo, hi)      MIN(MAX((x), (lo)), (hi))

#define IS_POWER_OF_2(x)      ((x) && !((x) & ((x) - 1)))

#define __ALIGN_MASK(x, mask) (((x) + (mask)) & ~(mask))

#define ALIGN_UP(x, a)                                                         \
    ({                                                                         \
        __auto_type _x = (x);                                                  \
        __auto_type _a = (a);                                                  \
        __ALIGN_MASK(_x, _a - 1);                                              \
    })

#define ALIGN_DOWN(x, a)                                                       \
    ({                                                                         \
        __auto_type _x = (x);                                                  \
        __auto_type _a = (a);                                                  \
        _x & ~(_a - 1);                                                        \
    })

#define IS_ALIGNED(x, a)                                                       \
    ({                                                                         \
        __auto_type _x = (x);                                                  \
        __auto_type _a = (a);                                                  \
        ((_x & (_a - 1)) == 0);                                                \
    })

#define ROUND_UP(x, a)     (((x) + (a) - 1) & ~((a) - 1))
#define ROUND_DOWN(x, a)   ((x) & ~((a) - 1))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

#endif /* ifndef _NUVIX_MATH_H */
