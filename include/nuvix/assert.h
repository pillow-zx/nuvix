#ifndef _NUVIX_ASSERT_H
#define _NUVIX_ASSERT_H

#include <nuvix/compiler.h>

#define static_assert(expr, msg) _Static_assert(expr, msg)

#define BUILD_BUG_ON(cond) static_assert(!(cond), "BUILD_BUG_ON(" #cond ")")

__noreturn void __panic(const char *fmt, ...) __printf(1, 2);

#define panic(format, ...)                                                     \
    __panic("%s:%u (%s): " format, __FILE__, (unsigned int)__LINE__,           \
            __func__, ##__VA_ARGS__)

/* Both macros require literal formats; Assert always evaluates cond once. */
#define Assert(cond, format, ...)                                              \
    do {                                                                       \
        if (unlikely(!(cond)))                                                 \
            __panic("%s:%u (%s): assertion failed (%s): " format, __FILE__,    \
                    (unsigned int)__LINE__, __func__, #cond, ##__VA_ARGS__);   \
    } while (0)

#define TODO() panic("please implement me")

#endif /* ifndef _NUVIX_ASSERT_H */
