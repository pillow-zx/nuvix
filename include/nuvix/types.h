#ifndef _NUVIX_TYPES_H
#define _NUVIX_TYPES_H

/*
 * Compiler-provided fundamental types
 */

typedef __INT8_TYPE__           int8_t;
typedef __INT16_TYPE__          int16_t;
typedef __INT32_TYPE__          int32_t;
typedef __INT64_TYPE__          int64_t;

typedef __UINT8_TYPE__          uint8_t;
typedef __UINT16_TYPE__         uint16_t;
typedef __UINT32_TYPE__         uint32_t;
typedef __UINT64_TYPE__         uint64_t;

typedef __INT8_TYPE__           i8;
typedef __INT16_TYPE__          i16;
typedef __INT32_TYPE__          i32;
typedef __INT64_TYPE__          i64;

typedef __UINT8_TYPE__          u8;
typedef __UINT16_TYPE__         u16;
typedef __UINT32_TYPE__         u32;
typedef __UINT64_TYPE__         u64;


/*
 * Pointer-sized types
 */

typedef __UINTPTR_TYPE__        uintptr_t;
typedef __INTPTR_TYPE__         intptr_t;

typedef __SIZE_TYPE__           size_t;
typedef __PTRDIFF_TYPE__        ptrdiff_t;

typedef __PTRDIFF_TYPE__        ssize_t;

typedef size_t                  usize;
typedef ssize_t                 isize;


/*
 * Limits
 */

#define INT8_MAX                ((int8_t)(__INT8_MAX__))
#define INT16_MAX               ((int16_t)(__INT16_MAX__))
#define INT32_MAX               ((int32_t)(__INT32_MAX__))
#define INT64_MAX               ((int64_t)(__INT64_MAX__))

#define UINT8_MAX               ((uint8_t)__UINT8_MAX__)
#define UINT16_MAX              ((uint16_t)__UINT16_MAX__)
#define UINT32_MAX              ((uint32_t)__UINT32_MAX__)
#define UINT64_MAX              ((uint64_t)__UINT64_MAX__)

#define SIZE_MAX                ((size_t)-1)
#define SSIZE_MAX               ((ssize_t)(SIZE_MAX >> 1))

/*
 * Bool
 */

typedef _Bool                   bool;

#define true                    1
#define false                   0

/*
 * NULL
 */

#define NULL			((void *)0)

/*
 * Physical / virtual address
 */
typedef uintptr_t               paddr_t;
typedef uintptr_t               vaddr_t;

/*
 * Kernel / Process types
 */

typedef int64_t                 loff_t;
typedef int32_t                 pid_t;
typedef uint32_t                uid_t;
typedef uint32_t                gid_t;
typedef uint32_t                dev_t;

#endif
