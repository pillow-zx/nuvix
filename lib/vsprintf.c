/*
 * lib/vsprintf.c - 格式化输出（printk 底层）
 */

#include <nuvix/types.h>
#include <nuvix/printk.h>

static char *emit(char *buf, char *end, char c)
{
	if (buf < end)
		*buf = c;
	return buf + 1;
}

static char *emit_string(char *buf, char *end, const char *s, int width,
			 int left)
{
	size_t len = strlen(s);
	int pad = (width > (int)len) ? width - (int)len : 0;

	if (!left)
		while (pad-- > 0)
			buf = emit(buf, end, ' ');
	while (*s)
		buf = emit(buf, end, *s++);
	if (left)
		while (pad-- > 0)
			buf = emit(buf, end, ' ');

	return buf;
}

static char *emit_num(char *buf, char *end, uint64_t num, int base, int upper,
		      int width, int zero, int neg)
{
	static const char digits_lower[] = "0123456789abcdef";
	static const char digits_upper[] = "0123456789ABCDEF";
	const char *digits = upper ? digits_upper : digits_lower;

	char tmp[65];
	int nd = 0;


	if (num == 0)
		tmp[nd++] = '0';
	else {
		while (num) {
			tmp[nd++] = digits[num % base];
			num /= base;
		}
	}


	int total = nd + (neg ? 1 : 0);
	int pad = (width > total) ? width - total : 0;


	if (!zero && !neg)
		while (pad-- > 0)
			buf = emit(buf, end, ' ');


	if (neg)
		buf = emit(buf, end, '-');


	if (zero)
		while (pad-- > 0)
			buf = emit(buf, end, '0');


	if (!zero && neg)
		while (pad-- > 0)
			buf = emit(buf, end, ' ');


	while (--nd >= 0)
		buf = emit(buf, end, tmp[nd]);

	return buf;
}

enum len_mod { LEN_NONE, LEN_L, LEN_LL };

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	if (size == 0)
		return 0;

	char *p = buf;
	char *end = buf + size - 1;

	while (*fmt) {

		if (*fmt != '%') {
			p = emit(p, end, *fmt++);
			continue;
		}
		fmt++;


		int flag_zero = 0;
		int flag_left = 0;
		int flag_alt = 0;

		for (;;) {
			if (*fmt == '0')
				flag_zero = 1;
			else if (*fmt == '-')
				flag_left = 1;
			else if (*fmt == '#')
				flag_alt = 1;
			else
				break;
			fmt++;
		}


		if (flag_left)
			flag_zero = 0;


		int width = 0;
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');


		enum len_mod len = LEN_NONE;
		if (*fmt == 'l') {
			len = LEN_L;
			fmt++;
			if (*fmt == 'l') {
				len = LEN_LL;
				fmt++;
			}
		}


		char conv = *fmt++;

		switch (conv) {
		case '%':
			p = emit(p, end, '%');
			break;

		case 'c': {
			char c = (char)va_arg(ap, int);
			p = emit(p, end, c);
			break;
		}

		case 's': {
			const char *s = va_arg(ap, const char *);
			if (!s)
				s = "(null)";
			p = emit_string(p, end, s, width, flag_left);
			break;
		}

		case 'd': {
			int64_t num;
			if (len == LEN_LL)
				num = va_arg(ap, int64_t);
			else if (len == LEN_L)
				num = va_arg(ap, long);
			else
				num = va_arg(ap, int);

			int neg = (num < 0);
			uint64_t unum = neg ? -(uint64_t)num : (uint64_t)num;
			p = emit_num(p, end, unum, 10, 0, width, flag_zero,
				     neg);
			break;
		}

		case 'u': {
			uint64_t unum;
			if (len == LEN_LL)
				unum = va_arg(ap, uint64_t);
			else if (len == LEN_L)
				unum = va_arg(ap, uintptr_t);
			else
				unum = va_arg(ap, uint32_t);

			p = emit_num(p, end, unum, 10, 0, width, flag_zero, 0);
			break;
		}

		case 'x': {
			uint64_t unum;
			if (len == LEN_LL)
				unum = va_arg(ap, uint64_t);
			else if (len == LEN_L)
				unum = va_arg(ap, uintptr_t);
			else
				unum = va_arg(ap, uint32_t);

			if (flag_alt && unum != 0) {
				p = emit(p, end, '0');
				p = emit(p, end, 'x');
			}
			p = emit_num(p, end, unum, 16, 0, width, flag_zero, 0);
			break;
		}

		case 'X': {
			uint64_t unum;
			if (len == LEN_LL)
				unum = va_arg(ap, uint64_t);
			else if (len == LEN_L)
				unum = va_arg(ap, uintptr_t);
			else
				unum = va_arg(ap, uint32_t);

			if (flag_alt && unum != 0) {
				p = emit(p, end, '0');
				p = emit(p, end, 'X');
			}
			p = emit_num(p, end, unum, 16, 1, width, flag_zero, 0);
			break;
		}

		case 'p': {
			void *ptr = va_arg(ap, void *);
			uint64_t unum = (uintptr_t)ptr;

			p = emit(p, end, '0');
			p = emit(p, end, 'x');

			p = emit_num(p, end, unum, 16, 0, sizeof(void *) * 2, 1,
				     0);
			break;
		}

		default:

			p = emit(p, end, '%');
			p = emit(p, end, conv);
			break;
		}
	}


	if (p <= end)
		*p = '\0';
	else
		*end = '\0';

	return (int)(p - buf);
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{

	return vsnprintf(buf, (size_t)-1, fmt, ap);
}
