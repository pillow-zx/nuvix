/*
 * include/nuvix/printk.h - 内核日志、panic 与断言
 */

#ifndef _NUVIX_PRINTK_H
#define _NUVIX_PRINTK_H

#include <nuvix/types.h>
#include <nuvix/compiler.h>

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)	   __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)

#define LOG_ALL	    0
#define LOG_DEBUG   1
#define LOG_INFO    2
#define LOG_NOTICE  3
#define LOG_WARNING 4
#define LOG_ERROR   5

#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_INFO
#endif

void console_init_sbi(void);
void console_init_mmio(void);

size_t printk_log_buffer_size(void);
size_t printk_log_unread_size(void);
ssize_t printk_log_read(void *buffer, size_t size);
ssize_t printk_log_read_all(void *buffer, size_t size, bool clear);
void printk_log_clear(void);

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);

__printf(2, 3) __nonnull(2)
int __printk(int level, const char *fmt, ...) ;

__noreturn __printf(1, 2) __nonnull(1) __cold
void __panic(const char *fmt, ...);

#define _Log(level, ...)                                                       \
	do {                                                                   \
		if ((level) >= LOG_LEVEL)                                      \
			__printk((level), __VA_ARGS__);                        \
	} while (0)

#define printk(level, fmt, ...)                                                \
	do {                                                                   \
		_Log(level, fmt, ##__VA_ARGS__);                               \
	} while (0)

#define pr_err(fmt, ...)    printk(LOG_ERROR, fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)   printk(LOG_WARNING, fmt, ##__VA_ARGS__)
#define pr_notice(fmt, ...) printk(LOG_NOTICE, fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)   printk(LOG_INFO, fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...)  printk(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define panic(fmt, ...)	    __panic(fmt, ##__VA_ARGS__)

#define BUG_ON(cond)                                                           \
	do {                                                                   \
		if (unlikely(cond))                                            \
			panic("BUG: %s:%d %s\n", __FILE__, __LINE__, #cond);   \
	} while (0)

#define ASSERT(cond)                                                           \
	do {                                                                   \
		if (unlikely(!(cond)))                                         \
			panic("ASSERT: %s:%d %s\n", __FILE__, __LINE__,        \
			      #cond);                                          \
	} while (0)

#endif
