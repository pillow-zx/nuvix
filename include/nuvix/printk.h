#ifndef _NUVIX_PRINTK_H
#define _NUVIX_PRINTK_H

#include <nuvix/compiler.h>
#include <nuvix/stdarg.h>
#include <nuvix/macro.h>
#include <asm/sbi.h>

static inline int putchar(int c)
{
    return sbi_putchar(c);
}

static inline int getchar(void)
{
    int c;

    do {
        c = sbi_getchar();
    } while (c < 0);
    return c;
}

int vsprintf(char *buf, const char *fmt, va_list args) __printf(2, 0);
int vprintk(const char *fmt, va_list args) __printf(1, 0);
int printk(const char *fmt, ...) __printf(1, 2);

#define LOG_LEVEL_DEBUG   1
#define LOG_LEVEL_INFO    2
#define LOG_LEVEL_NOTICE  3
#define LOG_LEVEL_WARNING 4
#define LOG_LEVEL_ERROR   5

#define printk_level(level, fmt, ...)                                          \
    ((void)((level) >= CONFIG_LOG_LEVEL ? printk(fmt, ##__VA_ARGS__) : 0))

#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif

#define pr_debug(fmt, ...)                                                     \
    printk_level(LOG_LEVEL_DEBUG, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info(fmt, ...)                                                      \
    printk_level(LOG_LEVEL_INFO, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_notice(fmt, ...)                                                    \
    printk_level(LOG_LEVEL_NOTICE, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn(fmt, ...)                                                      \
    printk_level(LOG_LEVEL_WARNING, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err(fmt, ...)                                                       \
    printk_level(LOG_LEVEL_ERROR, pr_fmt(fmt), ##__VA_ARGS__)

#endif /* ifndef _NUVIX_PRINTK_H */
