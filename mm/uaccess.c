#include <nuvix/uaccess.h>
#include <nuvix/errno.h>
#include <nuvix/math.h>
#include <nuvix/string.h>
#include <nuvix/task.h>
#include <asm/uaccess.h>

#define USTR_CHUNK_SIZE 64

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

ssize_t strncpy_from_user(char *dst, const char *src, size_t count)
{
    size_t total = 0;

    while (total < count) {
        size_t left = PAGE_SIZE - ((uintptr_t)(src + total) & (PAGE_SIZE - 1));
        size_t chunk = MIN(MIN(count - total, (size_t)USTR_CHUNK_SIZE), left);
        size_t uncopied = copy_from_user(dst + total, src + total, chunk);
        size_t copied = chunk - uncopied;
        char *end = memchr(dst + total, '\0', copied);

        if (end)
            return end - dst;
        if (uncopied)
            return -EFAULT;
        total += chunk;
    }
    return count;
}
