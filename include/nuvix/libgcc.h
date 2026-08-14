#ifndef _NUVIX_LIBGCC_H
#define _NUVIX_LIBGCC_H

#include <nuvix/types.h>

int __ctzdi2(uint64_t x);
int __clzdi2(uint64_t x);
int __ffsdi2(uint64_t x);
int ffs(uint32_t x);

#endif
