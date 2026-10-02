#include <nuvix/ctypes.h>
#include <nuvix/errno.h>
#include <nuvix/printk.h>

int printk(const char *fmt, ...)
{
    char buf[1024];
    va_list args, num;
    int len;

    va_start(args, fmt);
    va_copy(num, args);
    len = vsprintf(NULL, fmt, num);
    va_end(num);
    if (len >= (int)sizeof(buf))
        len = -EOVERFLOW;
    if (len >= 0)
        len = vsprintf(buf, fmt, args);
    va_end(args);

    for (int i = 0; i < len; ++i) {
        if (buf[i] == '\n')
            putchar('\r');
        putchar((unsigned char)buf[i]);
    }
    return len;
}
