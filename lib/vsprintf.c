/*
 * lib/vsprintf.c - 格式化输出（printk 底层）
 */

#include <nuvix/types.h>
#include <nuvix/string.h>
#include <nuvix/printk.h>

static char *emit(char *buf, char *end, char c)
{
	if (end && buf < end)
		*buf = c;
	return buf + 1;
}

static char *emit_string(char *buf, char *end, const char *s, int width,
			 int left, int prec)
{
	size_t len = strlen(s);
	int pad;

	if (prec >= 0 && (size_t)prec < len)
		len = prec;
	pad = (width > (int)len) ? width - (int)len : 0;

	if (!left)
		while (pad-- > 0)
			buf = emit(buf, end, ' ');
	while (len-- > 0)
		buf = emit(buf, end, *s++);
	if (left)
		while (pad-- > 0)
			buf = emit(buf, end, ' ');

	return buf;
}

struct num_fmt {
	int base;
	int upper;
	int width;
	int prec;
	int zero;
	int left;
	int alt;
	int plus;
	int space;
	int neg;
};

static char *emit_num(char *buf, char *end, uint64_t num,
		      const struct num_fmt *f)
{
	static const char digits_lower[] = "0123456789abcdef";
	static const char digits_upper[] = "0123456789ABCDEF";
	const char *digits = f->upper ? digits_upper : digits_lower;

	uint64_t orig = num;
	char tmp[65];
	int nd = 0;
	int prec, zero, digit_pad, sign_len, alt_len, body, pad;
	char sign = 0;

	if (num == 0)
		tmp[nd++] = '0';
	else
		while (num) {
			tmp[nd++] = digits[num % f->base];
			num /= f->base;
		}

	prec = (f->prec >= 0) ? f->prec : 0;
	zero = f->zero && f->prec < 0;
	if (f->neg)
		sign = '-';
	else if (f->plus)
		sign = '+';
	else if (f->space)
		sign = ' ';
	sign_len = sign ? 1 : 0;

	alt_len = 0;
	if (f->alt && orig != 0) {
		if (f->base == 16)
			alt_len = 2;
		else if (f->base == 8)
			alt_len = 1;
	}

	digit_pad = (prec > nd) ? prec - nd : 0;
	body = sign_len + alt_len + digit_pad + nd;
	pad = (f->width > body) ? f->width - body : 0;

	if (!f->left && !zero)
		while (pad-- > 0)
			buf = emit(buf, end, ' ');

	if (sign)
		buf = emit(buf, end, sign);
	if (alt_len == 2) {
		buf = emit(buf, end, '0');
		buf = emit(buf, end, f->upper ? 'X' : 'x');
	} else if (alt_len == 1) {
		buf = emit(buf, end, '0');
	}
	if (zero)
		while (pad-- > 0)
			buf = emit(buf, end, '0');
	while (digit_pad-- > 0)
		buf = emit(buf, end, '0');
	while (--nd >= 0)
		buf = emit(buf, end, tmp[nd]);
	if (f->left)
		while (pad-- > 0)
			buf = emit(buf, end, ' ');

	return buf;
}

enum len_mod { LEN_NONE, LEN_L, LEN_LL, LEN_Z, LEN_J, LEN_T };

static int64_t va_arg_signed(va_list *ap, enum len_mod len)
{
	switch (len) {
	case LEN_LL:
	case LEN_J:
		return va_arg(*ap, long long);
	case LEN_L:
	case LEN_Z:
	case LEN_T:
		return va_arg(*ap, long);
	case LEN_NONE:
	default:
		return va_arg(*ap, int);
	}
}

static uint64_t va_arg_unsigned(va_list *ap, enum len_mod len)
{
	switch (len) {
	case LEN_LL:
	case LEN_J:
		return va_arg(*ap, unsigned long long);
	case LEN_L:
	case LEN_Z:
	case LEN_T:
		return va_arg(*ap, unsigned long);
	case LEN_NONE:
	default:
		return va_arg(*ap, uint32_t);
	}
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	char *p = buf;
	char *end = size ? buf + size - 1 : NULL;

	while (*fmt) {
		if (*fmt != '%') {
			p = emit(p, end, *fmt++);
			continue;
		}
		fmt++;

		int flag_zero = 0;
		int flag_left = 0;
		int flag_alt = 0;
		int flag_plus = 0;
		int flag_space = 0;

		for (;;) {
			if (*fmt == '0')
				flag_zero = 1;
			else if (*fmt == '-')
				flag_left = 1;
			else if (*fmt == '#')
				flag_alt = 1;
			else if (*fmt == '+')
				flag_plus = 1;
			else if (*fmt == ' ')
				flag_space = 1;
			else
				break;
			fmt++;
		}

		if (flag_left)
			flag_zero = 0;
		if (flag_plus)
			flag_space = 0;

		int width = 0;
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');

		int prec = -1;
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			while (*fmt >= '0' && *fmt <= '9')
				prec = prec * 10 + (*fmt++ - '0');
		}

		enum len_mod len = LEN_NONE;
		if (*fmt == 'l') {
			len = LEN_L;
			fmt++;
			if (*fmt == 'l') {
				len = LEN_LL;
				fmt++;
			}
		} else if (*fmt == 'z') {
			len = LEN_Z;
			fmt++;
		} else if (*fmt == 'j') {
			len = LEN_J;
			fmt++;
		} else if (*fmt == 't') {
			len = LEN_T;
			fmt++;
		}

		char conv = *fmt++;
		struct num_fmt nf = {
			.width = width,
			.prec = prec,
			.zero = flag_zero,
			.left = flag_left,
			.alt = flag_alt,
			.plus = flag_plus,
			.space = flag_space,
		};

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
			p = emit_string(p, end, s, width, flag_left, prec);
			break;
		}

		case 'd':
		case 'i': {
			int64_t num = va_arg_signed(&ap, len);
			nf.neg = num < 0;
			nf.base = 10;
			nf.upper = 0;
			p = emit_num(p, end,
				     nf.neg ? -(uint64_t)num : (uint64_t)num,
				     &nf);
			break;
		}

		case 'u':
			nf.base = 10;
			nf.upper = 0;
			p = emit_num(p, end, va_arg_unsigned(&ap, len), &nf);
			break;

		case 'o':
			nf.base = 8;
			nf.upper = 0;
			p = emit_num(p, end, va_arg_unsigned(&ap, len), &nf);
			break;

		case 'x':
			nf.base = 16;
			nf.upper = 0;
			p = emit_num(p, end, va_arg_unsigned(&ap, len), &nf);
			break;

		case 'X':
			nf.base = 16;
			nf.upper = 1;
			p = emit_num(p, end, va_arg_unsigned(&ap, len), &nf);
			break;

		case 'p': {
			void *ptr = va_arg(ap, void *);
			uint64_t unum = (uintptr_t)ptr;
			struct num_fmt pf = {
				.base = 16,
				.width = sizeof(void *) * 2,
				.prec = -1,
				.zero = 1,
			};
			p = emit(p, end, '0');
			p = emit(p, end, 'x');
			p = emit_num(p, end, unum, &pf);
			break;
		}

		default:
			p = emit(p, end, '%');
			p = emit(p, end, conv);
			break;
		}
	}

	if (end) {
		if (p <= end)
			*p = '\0';
		else
			*end = '\0';
	}

	return (int)(p - buf);
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
	return vsnprintf(buf, (size_t)-1, fmt, ap);
}
