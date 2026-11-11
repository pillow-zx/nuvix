#ifndef _NUVIX_RISCV_UACCESS_H
#define _NUVIX_RISCV_UACCESS_H

#include <nuvix/ctypes.h>

/* Internal primitives: caller checks the user range and current address space. */
size_t __copy_from_user(void *dst, const void *src, size_t size);
size_t __copy_to_user(void *dst, const void *src, size_t size);

extern char __uaccess_load8[], __uaccess_load16[];
extern char __uaccess_load32[], __uaccess_load64[];
extern char __uaccess_store8[], __uaccess_store16[];
extern char __uaccess_store32[], __uaccess_store64[];
extern char __uaccess_fixup[];

#endif /* _NUVIX_RISCV_UACCESS_H */
