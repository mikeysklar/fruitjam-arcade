/* fruitjam-scumm - SCUMM v3/v4 on the Adafruit Fruit Jam
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * vsnprintf for the natmod build: d i u x X o c s p f and %%, with flags,
 * width, precision and the h/l/ll/z length modifiers. Floats are printed
 * as fixed point with at most 9 decimals (only debug text uses them).
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int is_digit(char c) {
	return c >= '0' && c <= '9';
}

struct out {
	char *buf;
	size_t size, len;
};

static void put(struct out *o, char c) {
	if (o->len + 1 < o->size)
		o->buf[o->len] = c;
	o->len++;
}

static void pad(struct out *o, char c, int n) {
	while (n-- > 0)
		put(o, c);
}

/* Emit a converted field: optional sign/prefix, zero padding, digits. */
static void field(struct out *o, const char *prefix, const char *digits, int ndigits, int width, int left, int zero) {
	int np = strlen(prefix);
	int total = np + ndigits;
	if (!left && !zero)
		pad(o, ' ', width - total);
	while (*prefix)
		put(o, *prefix++);
	if (!left && zero)
		pad(o, '0', width - total);
	for (int i = 0; i < ndigits; i++)
		put(o, digits[i]);
	if (left)
		pad(o, ' ', width - total);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
	struct out o = { buf, size, 0 };
	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			put(&o, *fmt);
			continue;
		}
		fmt++;
		int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
		for (;; fmt++) {
			if (*fmt == '-')
				left = 1;
			else if (*fmt == '0')
				zero = 1;
			else if (*fmt == '+')
				plus = 1;
			else if (*fmt == ' ')
				space = 1;
			else if (*fmt == '#')
				alt = 1;
			else
				break;
		}
		int width = 0, prec = -1;
		if (*fmt == '*') {
			width = va_arg(ap, int);
			if (width < 0) {
				left = 1;
				width = -width;
			}
			fmt++;
		} else {
			while (is_digit(*fmt))
				width = width * 10 + *fmt++ - '0';
		}
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			if (*fmt == '*') {
				prec = va_arg(ap, int);
				fmt++;
			} else {
				while (is_digit(*fmt))
					prec = prec * 10 + *fmt++ - '0';
			}
		}
		int lng = 0;
		for (;; fmt++) {
			if (*fmt == 'l')
				lng++;
			else if (*fmt == 'h' || *fmt == 'z' || *fmt == 't' || *fmt == 'j')
				;
			else
				break;
		}
		char tmp[32];
		const char *prefix = "";
		int n = 0;
		switch (*fmt) {
		case 'd':
		case 'i':
		case 'u':
		case 'x':
		case 'X':
		case 'o':
		case 'p': {
			unsigned long long v;
			int neg = 0;
			int base = 10;
			const char *dig = "0123456789abcdef";
			if (*fmt == 'p') {
				v = (uintptr_t)va_arg(ap, void *);
				base = 16;
				prefix = "0x";
			} else if (*fmt == 'd' || *fmt == 'i') {
				long long s = lng >= 2 ? va_arg(ap, long long) : lng ? va_arg(ap, long) : va_arg(ap, int);
				neg = s < 0;
				v = neg ? -(unsigned long long)s : (unsigned long long)s;
				prefix = neg ? "-" : plus ? "+" : space ? " " : "";
			} else {
				v = lng >= 2 ? va_arg(ap, unsigned long long) : lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
				if (*fmt == 'x' || *fmt == 'X')
					base = 16;
				else if (*fmt == 'o')
					base = 8;
				if (*fmt == 'X')
					dig = "0123456789ABCDEF";
				if (alt && v && base == 16)
					prefix = *fmt == 'X' ? "0X" : "0x";
			}
			char rev[24];
			int r = 0;
			while (v) {
				rev[r++] = dig[v % base];
				v /= base;
			}
			if (prec < 0 && !r)
				rev[r++] = '0';
			while (r < prec && r < (int)sizeof(rev))
				rev[r++] = '0';
			for (n = 0; n < r; n++)
				tmp[n] = rev[r - 1 - n];
			field(&o, prefix, tmp, n, width, left, zero && prec < 0);
			break;
		}
		case 'f':
		case 'g':
		case 'e': {
			double d = va_arg(ap, double);
			if (prec < 0)
				prec = 6;
			if (prec > 9)
				prec = 9;
			int neg = d < 0;
			if (neg)
				d = -d;
			unsigned long scale = 1;
			for (int i = 0; i < prec; i++)
				scale *= 10;
			unsigned long long fixed = (unsigned long long)(d * scale + 0.5);
			unsigned long long ip = fixed / scale, fp = fixed % scale;
			char rev[32];
			int r = 0;
			for (int i = 0; i < prec; i++) {
				rev[r++] = '0' + fp % 10;
				fp /= 10;
			}
			if (prec)
				rev[r++] = '.';
			do {
				rev[r++] = '0' + ip % 10;
				ip /= 10;
			} while (ip && r < 31);
			for (n = 0; n < r; n++)
				tmp[n] = rev[r - 1 - n];
			field(&o, neg ? "-" : plus ? "+" : "", tmp, n, width, left, zero);
			break;
		}
		case 'c':
			tmp[0] = (char)va_arg(ap, int);
			field(&o, "", tmp, 1, width, left, 0);
			break;
		case 's': {
			const char *s = va_arg(ap, const char *);
			if (!s)
				s = "(null)";
			int len = 0;
			while (s[len] && (prec < 0 || len < prec))
				len++;
			field(&o, "", s, len, width, left, 0);
			break;
		}
		case '%':
			put(&o, '%');
			break;
		case 0:
			fmt--;
			break;
		default:
			put(&o, '%');
			put(&o, *fmt);
			break;
		}
	}
	if (o.size)
		o.buf[o.len < o.size ? o.len : o.size - 1] = 0;
	return (int)o.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

int sprintf(char *buf, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, (size_t)-1 >> 1, fmt, ap);
	va_end(ap);
	return n;
}

