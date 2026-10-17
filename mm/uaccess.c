#include <nuvix/uaccess.h>
#include <nuvix/string.h>
#include <nuvix/task.h>
#include <asm/uaccess.h>

size_t copy_from_user(void *dst, const void *src, size_t size)
{
    size_t remaining = size;

    if (!size)
        return 0;
    if (access_ok(src, size) && current->mm)
        remaining = __copy_from_user(dst, src, size);
    if (remaining)
        memset((unsigned char *)dst + (size - remaining), 0, remaining);
    return remaining;
}

size_t copy_to_user(void *dst, const void *src, size_t size)
{
    if (!size)
        return 0;
    if (!access_ok(dst, size) || !current->mm)
        return size;
    return __copy_to_user(dst, src, size);
}
