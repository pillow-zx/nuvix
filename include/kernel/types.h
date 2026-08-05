#ifndef _CUTEOS_KERNEL_TYPES_H
#define _CUTEOS_KERNEL_TYPES_H

/*
 * include/kernel/types.h - 定宽整数类型与基础宏
 */

typedef signed char		int8_t;
typedef signed short		int16_t;
typedef signed int		int32_t;
typedef signed long long	int64_t;
typedef signed char		i8;
typedef signed short		i16;
typedef signed int		i32;
typedef signed long long	i64;

typedef unsigned char		uint8_t;
typedef unsigned short		uint16_t;
typedef unsigned int		uint32_t;
typedef unsigned long long	uint64_t;

typedef unsigned char		u8;
typedef unsigned short		u16;
typedef unsigned int		u32;
typedef unsigned long long	u64;

typedef unsigned long		uintptr_t;
typedef signed long		ptrdiff_t;

typedef unsigned long		size_t;
typedef signed long		ssize_t;
typedef unsigned long           usize;
typedef signed long             isize;

typedef int64_t			loff_t;
typedef int32_t			pid_t;
typedef uint32_t		uid_t;
typedef uint32_t		gid_t;
typedef uint32_t		dev_t;

typedef uintptr_t		paddr_t;
typedef uintptr_t		vaddr_t;

typedef _Bool                   bool ;

#define false                   0
#define true                    1

#if __STDC_VERSION__ >= 202311L
#define NULL			nullptr
#else
#define NULL	                ((void *)0)
#endif

#define INT8_MIN                (-1 - 0x7f)
#define INT16_MIN               (-1 - 0x7fff)
#define INT32_MIN               (-1 - 0x7fffffff)
#define INT64_MIN               (-1 - 0x7fffffffffffffff)

#define INT8_MAX                (0x7f)
#define INT16_MAX               (0x7fff)
#define INT32_MAX               (0x7fffffff)
#define INT64_MAX               (0x7fffffffffffffff)

#define UINT8_MAX               (0xff)
#define UINT16_MAX              (0xffff)
#define UINT32_MAX              (0xffffffffu)
#define UINT64_MAX              (0xffffffffffffffffu)

#endif
