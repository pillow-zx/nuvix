#ifndef _NUVIX_UACCESS_H
#define _NUVIX_UACCESS_H

#include <nuvix/compiler.h>
#include <nuvix/ctypes.h>
#include <nuvix/layout.h>

/* Range check only; mapping and permissions are checked by the actual access. */
static inline bool access_ok(const void *addr, size_t size)
{
    uintptr_t start = (uintptr_t)addr;

    return start >= UVADDR_START && start < UVADDR_END &&
           size <= UVADDR_END - start;
}

/*
 * Copy in the current user task's address space; kernel buffers must be valid
 * for size bytes. Return the number of bytes not copied, including size for an
 * invalid user range or a kernel task. Zero size succeeds without accessing
 * either pointer. copy_from_user zeroes the destination
 * when rejecting the copy. Neither operation guarantees a coherent snapshot.
 */
size_t __must_check copy_from_user(void *dst, const void *src, size_t size);
size_t __must_check copy_to_user(void *dst, const void *src, size_t size);

#endif /* _NUVIX_UACCESS_H */
