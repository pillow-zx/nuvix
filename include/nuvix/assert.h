#ifndef _NUVIX_ASSERT_H
#define _NUVIX_ASSERT_H

#define static_assert(expr, msg) _Static_assert(expr, msg)

#define BUILD_BUG_ON(cond) static_assert(!(cond), "BUILD_BUG_ON(" #cond ")")

#endif /* ifndef _NUVIX_ASSERT_H */
