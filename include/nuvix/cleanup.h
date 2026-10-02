#ifndef _NUVIX_CLEANUP_H
#define _NUVIX_CLEANUP_H

#include <nuvix/compiler.h>

#define CLEANUP_DEFINE(_name, _type, _cleanup)                                 \
    static inline void __cleanup_##_name(void *p)                              \
    {                                                                          \
        _type _T = *(_type *)p;                                                \
        _cleanup;                                                              \
    }

#define __cleanup_with(_name) __cleanup(__cleanup_##_name)

#endif
