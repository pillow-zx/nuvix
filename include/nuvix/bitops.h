#ifndef _NUVIX_BITOPS_H
#define _NUVIX_BITOPS_H

#define BIT(n)     (1UL << (n))
#define BIT_ULL(n) (1ULL << (n))

#define GENMASK(h, l)                                                          \
    (((~0UL) << (l)) & (~0UL >> (sizeof(unsigned long) * 8 - 1 - (h))))

#define BIT_SET(v, n)   ((v) |= BIT(n))
#define BIT_CLEAR(v, n) ((v) &= ~BIT(n))
#define BIT_TEST(v, n)  (!!((v) & BIT(n)))

#endif /* ifndef _NUVIX_BITOPS_H */
