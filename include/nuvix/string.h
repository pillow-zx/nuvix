#ifndef _NUVIX_STRING_H
#define _NUVIX_STRING_H

#include <nuvix/types.h>

extern void *memcpy(void *restrict dst, const void *restrict src, unsigned long n);

extern void *memset(void *dst, int c, unsigned long n);

extern int memcmp(const void *lsh, const void *rhs, unsigned long n);

extern void *memmove(void *dst, const void *src, unsigned long n);

extern unsigned long strlen(const char *s);

extern unsigned long strnlen(const char *s, const unsigned long maxlen);

extern int strcmp(const char *lhs, const char *rhs);

extern int strncmp(const char *lhs, const char *rhs, unsigned long n);

extern char *strcpy(char *restrict dst, const char *restrict src);

extern char *strncpy(char *restrict dst, const char *restrict src, unsigned long n);

extern char *strchr(const char *s, int c);

extern char *strrchr(const char *s, int c);

void memswap(void *restrict lhs, void *restrict rhs, size_t size);

#endif
