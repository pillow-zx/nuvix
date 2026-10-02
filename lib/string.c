#include <nuvix/string.h>

void *memcpy(void *restrict dst, const void *restrict src, unsigned long n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, unsigned long n)
{
    unsigned char *d = dst;

    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *lhs, const void *rhs, unsigned long n)
{
    const unsigned char *l = lhs, *r = rhs;

    for (; n && *l == *r; n--, l++, r++)
        ;
    return n ? *l - *r : 0;
}

void *memmove(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (d == s || !n)
        return dst;
    if ((uintptr_t)d < (uintptr_t)s) {
        while (n--)
            *d++ = *s++;
    } else {
        while (n) {
            n--;
            d[n] = s[n];
        }
    }
    return dst;
}

unsigned long strlen(const char *s)
{
    const char *start = s;

    while (*s)
        s++;
    return s - start;
}

int strcmp(const char *lhs, const char *rhs)
{
    while (*lhs && *lhs == *rhs) {
        lhs++;
        rhs++;
    }
    return *(const unsigned char *)lhs - *(const unsigned char *)rhs;
}

char *strcpy(char *restrict dst, const char *restrict src)
{
    char *start = dst;

    while ((*dst++ = *src++))
        ;
    return start;
}

char *strchr(const char *s, int c)
{
    do {
        if ((unsigned char)*s == (unsigned char)c)
            return (char *)s;
    } while (*s++);
    return NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;

    do {
        if ((unsigned char)*s == (unsigned char)c)
            last = s;
    } while (*s++);
    return (char *)last;
}

void *memchr(const void *s, int c, unsigned long n)
{
    const unsigned char *p = s;

    while (n--) {
        if (*p == (unsigned char)c)
            return (void *)p;
        p++;
    }
    return NULL;
}

unsigned long strnlen(const char *s, unsigned long n)
{
    unsigned long len = 0;

    while (len < n && s[len])
        len++;
    return len;
}
