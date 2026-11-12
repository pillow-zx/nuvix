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
static int number(char *buf, int *len, unsigned long long value,
                  unsigned int base)
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
            bool alternate = false;
            unsigned int length = 0;

            ++fmt;
            if (*fmt == '#') {
                alternate = true;
                ++fmt;
            }
            while (length < 2 && *fmt == 'l') {
                ++length;
                ++fmt;
            }
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
                long long value;
                unsigned long long magnitude;

                if (length == 2)
                    value = va_arg(args, long long);
                else if (length == 1)
                    value = va_arg(args, long);
                else
                    value = va_arg(args, int);
                magnitude = (unsigned long long)value;
                if (value < 0) {
                    ret = append(buf, &len, '-');
                    /* Unsigned subtraction also handles the signed minimum. */
                    magnitude = 0ULL - magnitude;
                }
                if (ret == 0)
                    ret = number(buf, &len, magnitude, 10);
                break;
            }
            case 'u':
            case 'x': {
                unsigned long long value;
                unsigned int base = *fmt == 'x' ? 16 : 10;

                if (length == 2)
                    value = va_arg(args, unsigned long long);
                else if (length == 1)
                    value = va_arg(args, unsigned long);
                else
                    value = va_arg(args, unsigned int);
                if (alternate && base == 16 && value) {
                    ret = append(buf, &len, '0');
                    if (ret == 0)
                        ret = append(buf, &len, 'x');
                }
                if (ret == 0)
                    ret = number(buf, &len, value, base);
                break;
            }
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
