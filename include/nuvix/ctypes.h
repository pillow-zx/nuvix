#ifndef _NUVIX_CTYPES_H
#define _NUVIX_CTYPES_H

typedef __INT8_TYPE__ int8_t;
typedef __INT16_TYPE__ int16_t;
typedef __INT32_TYPE__ int32_t;
typedef __INT64_TYPE__ int64_t;

typedef __UINT8_TYPE__ uint8_t;
typedef __UINT16_TYPE__ uint16_t;
typedef __UINT32_TYPE__ uint32_t;
typedef __UINT64_TYPE__ uint64_t;

typedef __INTPTR_TYPE__ intptr_t;
typedef __UINTPTR_TYPE__ uintptr_t;

#define INT8_MAX   ((int8_t)(__INT8_MAX__))
#define INT16_MAX  ((int16_t)(__INT16_MAX__))
#define INT32_MAX  ((int32_t)(__INT32_MAX__))
#define INT64_MAX  ((int64_t)(__INT64_MAX__))

#define UINT8_MAX  ((uint8_t)__UINT8_MAX__)
#define UINT16_MAX ((uint16_t)__UINT16_MAX__)
#define UINT32_MAX ((uint32_t)__UINT32_MAX__)
#define UINT64_MAX ((uint64_t)__UINT64_MAX__)

#define NULL       ((void *)0)

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ssize_t;

#define SIZE_MAX  ((size_t)-1)
#define SSIZE_MAX ((ssize_t)(SIZE_MAX >> 1))

#define bool      _Bool
#define true      1
#define false     0

typedef uintptr_t vaddr_t;
typedef uint64_t paddr_t;

#endif /* ifndef _NUVIX_CTYPES_H */
