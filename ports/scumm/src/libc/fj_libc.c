/* fruitjam-scumm - SCUMM v3/v4 on the Adafruit Fruit Jam
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * The few C library functions the engine needs, for the natmod build
 * (there is no libc in a native module; memcpy, memmove and memset come
 * from MicroPython through mpy_ld, malloc and friends from fj_alloc.c).
 * Plain C locale. printf is in fj_printf.c.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

size_t strlen(const char *s) {
	const char *p = s;
	while (*p)
		p++;
	return p - s;
}

int strcmp(const char *a, const char *b) {
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
	for (; n; n--, a++, b++) {
		if (*a != *b || !*a)
			return (unsigned char)*a - (unsigned char)*b;
	}
	return 0;
}

char *strcpy(char *d, const char *s) {
	char *r = d;
	while ((*d++ = *s++))
		;
	return r;
}

char *strncpy(char *d, const char *s, size_t n) {
	char *r = d;
	for (; n && *s; n--)
		*d++ = *s++;
	for (; n; n--)
		*d++ = 0;
	return r;
}

char *strcat(char *d, const char *s) {
	strcpy(d + strlen(d), s);
	return d;
}

char *strchr(const char *s, int c) {
	for (;; s++) {
		if (*s == (char)c)
			return (char *)s;
		if (!*s)
			return 0;
	}
}

char *strrchr(const char *s, int c) {
	const char *r = 0;
	for (;; s++) {
		if (*s == (char)c)
			r = s;
		if (!*s)
			return (char *)r;
	}
}

char *strstr(const char *h, const char *n) {
	size_t ln = strlen(n);
	if (!ln)
		return (char *)h;
	for (; *h; h++) {
		if (*h == *n && !strncmp(h, n, ln))
			return (char *)h;
	}
	return 0;
}

void *memchr(const void *s, int c, size_t n) {
	const unsigned char *p = s;
	for (; n; n--, p++) {
		if (*p == (unsigned char)c)
			return (void *)p;
	}
	return 0;
}

int memcmp(const void *a, const void *b, size_t n) {
	const unsigned char *p = a, *q = b;
	for (; n; n--, p++, q++) {
		if (*p != *q)
			return *p - *q;
	}
	return 0;
}

int isspace(int c) {
	return c == ' ' || (c >= '\t' && c <= '\r');
}

int isdigit(int c) {
	return c >= '0' && c <= '9';
}

int isalpha(int c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

int isalnum(int c) {
	return isalpha(c) || isdigit(c);
}

int isupper(int c) {
	return c >= 'A' && c <= 'Z';
}

int islower(int c) {
	return c >= 'a' && c <= 'z';
}

int isprint(int c) {
	return c >= 0x20 && c < 0x7f;
}

int ispunct(int c) {
	return isprint(c) && !isalnum(c) && c != ' ';
}

int isxdigit(int c) {
	return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int toupper(int c) {
	return islower(c) ? c - 32 : c;
}

int tolower(int c) {
	return isupper(c) ? c + 32 : c;
}

long strtol(const char *s, char **end, int base) {
	while (isspace(*s))
		s++;
	int neg = 0;
	if (*s == '-' || *s == '+')
		neg = *s++ == '-';
	if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
		base = 16;
	} else if (base == 0) {
		base = s[0] == '0' ? 8 : 10;
	}
	unsigned long v = 0;
	for (;; s++) {
		int d;
		if (isdigit(*s))
			d = *s - '0';
		else if (isalpha(*s))
			d = tolower(*s) - 'a' + 10;
		else
			break;
		if (d >= base)
			break;
		v = v * base + d;
	}
	if (end)
		*end = (char *)s;
	return neg ? -(long)v : (long)v;
}

unsigned long strtoul(const char *s, char **end, int base) {
	return (unsigned long)strtol(s, end, base);
}

int atoi(const char *s) {
	return (int)strtol(s, 0, 10);
}

long atol(const char *s) {
	return strtol(s, 0, 10);
}

int abs(int x) {
	return x < 0 ? -x : x;
}

/* ---- setjmp/longjmp: r4-r11, sp, lr, s16-s31 (jmp_buf is big enough) ---- */

#if defined(__arm__)
__asm__(
	"	.text\n"
	"	.thumb\n"
	"	.syntax unified\n"
	"	.global setjmp\n"
	"	.type setjmp, %function\n"
	"	.thumb_func\n"
	"setjmp:\n"
	"	mov ip, sp\n"
	"	stmia r0!, {r4-r11, ip, lr}\n"
#if defined(__ARM_FP)
	"	vstmia r0!, {s16-s31}\n"
#endif
	"	movs r0, #0\n"
	"	bx lr\n"
	"	.size setjmp, . - setjmp\n"
	"	.global longjmp\n"
	"	.type longjmp, %function\n"
	"	.thumb_func\n"
	"longjmp:\n"
	"	ldmia r0!, {r4-r11, ip, lr}\n"
#if defined(__ARM_FP)
	"	vldmia r0!, {s16-s31}\n"
#endif
	"	mov sp, ip\n"
	"	movs r0, r1\n"
	"	it eq\n"
	"	moveq r0, #1\n"
	"	bx lr\n"
	"	.size longjmp, . - longjmp\n"
);
#endif
