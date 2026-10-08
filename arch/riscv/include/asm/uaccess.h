#ifndef _NUVIX_RISCV_UACCESS_H
#define _NUVIX_RISCV_UACCESS_H

#include <nuvix/ctypes.h>

/* Internal primitives: caller checks the user range and current address space. */
size_t __copy_from_user(void *dst, const void *src, size_t size);
size_t __copy_to_user(void *dst, const void *src, size_t size);

#endif /* _NUVIX_RISCV_UACCESS_H */
