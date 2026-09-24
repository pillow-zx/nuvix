#ifndef _NUVIX_ARCH_RISCV_UACCESS_H
#define _NUVIX_ARCH_RISCV_UACCESS_H

#include <arch/page.h>
#include <nuvix/compiler.h>
#include <nuvix/errno.h>
#include <nuvix/types.h>

#define __RISCV_UACCESS_EX_TABLE                                               \
	".pushsection __ex_table,\"a\"\n\t"                                    \
	".balign 8\n\t"                                                        \
	".dword 1b, 2b\n\t"                                                    \
	".popsection\n\t"

__always_inline __must_check
static inline int __riscv_user_access_check(const volatile void *addr, size_t width)
{
	uintptr_t start = (uintptr_t)addr;

	if (!addr || width == 0)
		return -EFAULT;
	if ((start & (width - 1)) != 0)
		return -EINVAL;
	if (start >= TASK_SIZE || width > TASK_SIZE - start)
		return -EFAULT;
	return 0;
}

__always_inline __must_check
static inline int __riscv_user_output_check(const void *out, size_t width)
{
	if (!out || (width > 1 && ((uintptr_t)out & (width - 1)) != 0))
		return -EINVAL;
	return 0;
}

#define __RISCV_DEFINE_USER_GET(width, type, instruction)                      \
	__always_inline __must_check static inline int                         \
	__riscv_user_get_u##width(type *out, const volatile type *addr)        \
	{                                                                      \
		int __ret = __riscv_user_access_check(addr, sizeof(type));     \
		register uintptr_t __a0 asm("a0") = (uintptr_t)addr;           \
		register uintptr_t __a1 asm("a1");                             \
                                                                               \
		if (__ret < 0)                                                 \
			return __ret;                                          \
		__ret = __riscv_user_output_check(out, sizeof(type));          \
		if (__ret < 0)                                                 \
			return __ret;                                          \
		asm volatile("csrr t1, sstatus\n\t"                            \
			     "li t2, 0x40000\n\t"                              \
			     "or t2, t1, t2\n\t"                               \
			     "csrw sstatus, t2\n\t"                            \
			     "1:\n\t" #instruction " a1, 0(a0)\n\t"            \
			     "csrw sstatus, t1\n\t"                            \
			     "li a0, 0\n\t"                                    \
			     "j 3f\n\t"                                        \
			     "2:\n\t"                                          \
			     "csrw sstatus, t1\n\t"                            \
			     "3:\n\t" __RISCV_UACCESS_EX_TABLE                 \
			     : "+r"(__a0), "=r"(__a1)                          \
			     :                                                 \
			     : "t0", "t1", "t2", "memory");                    \
		if (__a0 != 0)                                                 \
			return -EFAULT;                                        \
		*out = (type)__a1;                                             \
		return 0;                                                      \
	}

#define __RISCV_DEFINE_USER_PUT(width, type, instruction)                      \
	__always_inline __must_check static inline int                         \
	__riscv_user_put_u##width(type value, volatile type *addr)             \
	{                                                                      \
		int __ret = __riscv_user_access_check(addr, sizeof(type));     \
		register uintptr_t __a0 asm("a0") = (uintptr_t)addr;           \
		register uintptr_t __a1 asm("a1") = (uintptr_t)value;          \
                                                                               \
		if (__ret < 0)                                                 \
			return __ret;                                          \
		asm volatile("csrr t1, sstatus\n\t"                            \
			     "li t2, 0x40000\n\t"                              \
			     "or t2, t1, t2\n\t"                               \
			     "csrw sstatus, t2\n\t"                            \
			     "1:\n\t" #instruction " a1, 0(a0)\n\t"            \
			     "csrw sstatus, t1\n\t"                            \
			     "li a0, 0\n\t"                                    \
			     "j 3f\n\t"                                        \
			     "2:\n\t"                                          \
			     "csrw sstatus, t1\n\t"                            \
			     "3:\n\t" __RISCV_UACCESS_EX_TABLE                 \
			     : "+r"(__a0)                                      \
			     : "r"(__a1)                                       \
			     : "t0", "t1", "t2", "memory");                    \
		return (int)__a0;                                              \
	}

__RISCV_DEFINE_USER_GET(8, u8, lbu)
__RISCV_DEFINE_USER_GET(16, u16, lhu)
__RISCV_DEFINE_USER_GET(32, u32, lwu)
__RISCV_DEFINE_USER_GET(64, u64, ld)

__RISCV_DEFINE_USER_PUT(8, u8, sb)
__RISCV_DEFINE_USER_PUT(16, u16, sh)
__RISCV_DEFINE_USER_PUT(32, u32, sw)
__RISCV_DEFINE_USER_PUT(64, u64, sd)

#undef __RISCV_DEFINE_USER_GET
#undef __RISCV_DEFINE_USER_PUT

__always_inline __must_check
static inline int __riscv_user_get(void *out, const volatile void *addr, size_t width)
{
	switch (width) {
	case sizeof(u8): {
		u8 value;
		int ret =
			__riscv_user_get_u8(&value, (const volatile u8 *)addr);

		if (ret == 0)
			*(u8 *)out = value;
		return ret;
	}
	case sizeof(u16): {
		u16 value;
		int ret = __riscv_user_get_u16(&value,
					       (const volatile u16 *)addr);

		if (ret == 0)
			*(u16 *)out = value;
		return ret;
	}
	case sizeof(u32): {
		u32 value;
		int ret = __riscv_user_get_u32(&value,
					       (const volatile u32 *)addr);

		if (ret == 0)
			*(u32 *)out = value;
		return ret;
	}
	case sizeof(u64): {
		u64 value;
		int ret = __riscv_user_get_u64(&value,
					       (const volatile u64 *)addr);

		if (ret == 0)
			*(u64 *)out = value;
		return ret;
	}
	default:
		return -EINVAL;
	}
}

__always_inline __must_check
static inline int __riscv_user_put(u64 value, volatile void *addr, size_t width)
{
	switch (width) {
	case sizeof(u8):
		return __riscv_user_put_u8((u8)value, (volatile u8 *)addr);
	case sizeof(u16):
		return __riscv_user_put_u16((u16)value, (volatile u16 *)addr);
	case sizeof(u32):
		return __riscv_user_put_u32((u32)value, (volatile u32 *)addr);
	case sizeof(u64):
		return __riscv_user_put_u64(value, (volatile u64 *)addr);
	default:
		return -EINVAL;
	}
}

#define __RISCV_USER_WIDTH_ASSERT(ptr)                                         \
	static_assert(sizeof(*(ptr)) == sizeof(u8) ||                          \
			      sizeof(*(ptr)) == sizeof(u16) ||                 \
			      sizeof(*(ptr)) == sizeof(u32) ||                 \
			      sizeof(*(ptr)) == sizeof(u64),                   \
		      "user access width must be u8/u16/u32/u64")

#define get_user(x, ptr)                                                       \
	statement_expr(__RISCV_USER_WIDTH_ASSERT(ptr);                         \
		       __riscv_user_get(&(x), (const volatile void *)(ptr),    \
					sizeof(*(ptr)));)

#define put_user(x, ptr)                                                       \
	statement_expr(__RISCV_USER_WIDTH_ASSERT(ptr);                         \
		       __riscv_user_put((u64)(x), (volatile void *)(ptr),      \
					sizeof(*(ptr)));)

#endif
