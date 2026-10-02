#ifndef _NUVIX_BITMAP_H
#define _NUVIX_BITMAP_H

#include <nuvix/ctypes.h>

#define BITS_PER_LONG (__CHAR_BIT__ * sizeof(unsigned long))
#define BITS_TO_LONGS(nbits)                                                   \
    ((nbits) / BITS_PER_LONG + !!((nbits) % BITS_PER_LONG))
#define DECLARE_BITMAP(name, nbits) unsigned long name[BITS_TO_LONGS(nbits)]
#define BIT_WORD(nr)                ((nr) / BITS_PER_LONG)
#define BIT_MASK(nr)                (1UL << ((nr) % BITS_PER_LONG))

static inline void set_bit(unsigned long nr, unsigned long *map)
{
    map[BIT_WORD(nr)] |= BIT_MASK(nr);
}

static inline void clear_bit(unsigned long nr, unsigned long *map)
{
    map[BIT_WORD(nr)] &= ~BIT_MASK(nr);
}

static inline bool test_bit(unsigned long nr, const unsigned long *map)
{
    return !!(map[BIT_WORD(nr)] & BIT_MASK(nr));
}

static inline void bitmap_zero(unsigned long *dst, unsigned long nbits)
{
    for (unsigned long i = 0; i < BITS_TO_LONGS(nbits); i++)
        dst[i] = 0;
}

static inline unsigned long find_next_bit(const unsigned long *map,
                                          unsigned long nbits,
                                          unsigned long start)
{
    for (; start < nbits; start++)
        if (test_bit(start, map))
            return start;
    return nbits;
}

static inline unsigned long find_next_zero_bit(const unsigned long *map,
                                               unsigned long nbits,
                                               unsigned long start)
{
    for (; start < nbits; start++)
        if (!test_bit(start, map))
            return start;
    return nbits;
}

static inline unsigned long find_first_bit(const unsigned long *map,
                                           unsigned long nbits)
{
    return find_next_bit(map, nbits, 0);
}

static inline unsigned long find_first_zero_bit(const unsigned long *map,
                                                unsigned long nbits)
{
    return find_next_zero_bit(map, nbits, 0);
}

#define for_each_set_bit(bit, map, nbits)                                      \
    for ((bit) = find_first_bit((map), (nbits)); (bit) < (nbits);              \
         (bit) = find_next_bit((map), (nbits), (bit) + 1))

#define for_each_clear_bit(bit, map, nbits)                                    \
    for ((bit) = find_first_zero_bit((map), (nbits)); (bit) < (nbits);         \
         (bit) = find_next_zero_bit((map), (nbits), (bit) + 1))

#endif /* _NUVIX_BITMAP_H */
