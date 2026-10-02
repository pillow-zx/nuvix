#include <nuvix/ctypes.h>
#include <nuvix/errno.h>
#include <nuvix/printk.h>

static int append(char *buf, int *len, char c)
{
    if (*len == __INT_MAX__)
        return -EOVERFLOW;
    if (buf)
        buf[*len] = c;
    ++*len;
    return 0;
}

/* Build digits backwards, then copy them in their printed order. */
static int number(char *buf, int *len, uintptr_t value, unsigned int base)
{
    const char digits[] = "0123456789abcdef";
    char tmp[3 * sizeof(value)];
    char *end = tmp + sizeof(tmp);
    char *p = end;

    do {
        *--p = digits[value % base];
        value /= base;
    } while (value);

    while (p != end) {
        int ret = append(buf, len, *p++);

        if (ret < 0)
            return ret;
    }
    return 0;
}

int vsprintf(char *buf, const char *fmt, va_list args)
{
    int len = 0;
    int ret = 0;

    while (*fmt) {
        if (*fmt != '%') {
            ret = append(buf, &len, *fmt++);
        } else {
            ++fmt;
            switch (*fmt) {
            case '%':
                ret = append(buf, &len, '%');
                break;
            case 'c':
                ret = append(buf, &len, (char)va_arg(args, int));
                break;
            case 's': {
                const char *s = va_arg(args, const char *);

                if (!s)
                    s = "(null)";
                while (*s) {
                    ret = append(buf, &len, *s++);
                    if (ret < 0)
                        break;
                }
                break;
            }
            case 'd': {
                int value = va_arg(args, int);
                unsigned int magnitude = (unsigned int)value;

                if (value < 0) {
                    ret = append(buf, &len, '-');
                    /* Unsigned subtraction also handles INT_MIN. */
                    magnitude = 0U - magnitude;
                }
                if (ret == 0)
                    ret = number(buf, &len, magnitude, 10);
                break;
            }
            case 'u':
                ret = number(buf, &len, va_arg(args, unsigned int), 10);
                break;
            case 'x':
                ret = number(buf, &len, va_arg(args, unsigned int), 16);
                break;
            case 'p':
                ret = append(buf, &len, '0');
                if (ret == 0)
                    ret = append(buf, &len, 'x');
                if (ret == 0)
                    ret =
                        number(buf, &len, (uintptr_t)va_arg(args, void *), 16);
                break;
            default:
                ret = -EINVAL;
                break;
            }
            if (*fmt)
                ++fmt;
        }
        if (ret < 0)
            break;
    }

    if (buf)
        buf[len] = '\0';
    return ret < 0 ? ret : len;
}
