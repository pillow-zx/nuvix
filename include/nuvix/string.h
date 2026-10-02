#ifndef _NUVIX_STRING_H
#define _NUVIX_STRING_H

#include <nuvix/ctypes.h>

void *memcpy(void *restrict dst, const void *restrict src, unsigned long n);

void *memset(void *dst, int c, unsigned long n);

int memcmp(const void *lsh, const void *rhs, unsigned long n);

void *memmove(void *dst, const void *src, unsigned long n);

unsigned long strlen(const char *s);

int strcmp(const char *lhs, const char *rhs);

char *strcpy(char *restrict dst, const char *restrict src);

char *strchr(const char *s, int c);

char *strrchr(const char *s, int c);

void *memchr(const void *s, int c, unsigned long n);

unsigned long strnlen(const char *s, unsigned long n);

#endif
