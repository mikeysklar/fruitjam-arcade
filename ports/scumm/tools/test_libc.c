/* Host unit test for the natmod allocator and printf (src/backend/fj_alloc.c,
 * src/libc/fj_printf.c), built with their symbols renamed:
 *   make -f Makefile.host test */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *t_malloc(size_t);
void t_free(void *);
void *t_realloc(void *, size_t);
void *t_calloc(size_t, size_t);
int t_vsnprintf(char *, size_t, const char *, va_list);
void fj_alloc_init(void *, size_t);
size_t fj_alloc_used(void);

static int fails;

static void check(const char *want, const char *fmt, ...) {
	char a[128];
	va_list ap;
	va_start(ap, fmt);
	t_vsnprintf(a, sizeof(a), fmt, ap);
	va_end(ap);
	if (strcmp(a, want)) {
		printf("printf \"%s\": got \"%s\" want \"%s\"\n", fmt, a, want);
		fails++;
	}
}

int main(void) {
	check("42 -7 ff FF 0x1f", "%d %i %x %X %#x", 42, -7, 255, 255, 31);
	check("007|  7|7  |", "%03d|%3d|%-3d|", 7, 7, 7);
	check("ab|   ab|ab   |a", "%s|%5s|%-5s|%.1s", "ab", "ab", "ab", "ab");
	check("1.5 0.333 -2.0", "%.1f %.3f %.1f", 1.5, 1.0 / 3, -2.0);
	check("4294967295 123456789012", "%u %lld", 4294967295u, 123456789012ll);
	check("%c=x", "%%c=%c", 'x');
	check("000a0b", "%04x%02x", 10, 11);
	check("-0042", "%05d", -42);

	static unsigned char arena[1 << 20];
	fj_alloc_init(arena, sizeof(arena));
	void *p[1000];
	size_t sz[1000];
	memset(p, 0, sizeof(p));
	srand(1);
	for (int it = 0; it < 200000; it++) {
		int i = rand() % 1000;
		if (p[i]) {
			unsigned char *q = p[i];
			for (size_t k = 0; k < sz[i]; k++) {
				if (q[k] != (unsigned char)(i + k)) {
					printf("corrupt block %d\n", i);
					return 1;
				}
			}
			if (rand() & 1) {
				t_free(p[i]);
				p[i] = 0;
				continue;
			}
			size_t n = 1 + rand() % 3000; /* 0 would free */
			q = t_realloc(p[i], n);
			if (!q)
				continue;
			p[i] = q;
			sz[i] = n;
			for (size_t k = 0; k < n; k++)
				q[k] = (unsigned char)(i + k);
		} else {
			size_t n = rand() % 4000;
			unsigned char *q = t_malloc(n);
			if (!q)
				continue;
			if (((size_t)q & 7)) {
				printf("misaligned\n");
				return 1;
			}
			p[i] = q;
			sz[i] = n;
			for (size_t k = 0; k < n; k++)
				q[k] = (unsigned char)(i + k);
		}
	}
	for (int i = 0; i < 1000; i++)
		t_free(p[i]);
	if (fj_alloc_used()) {
		printf("leak: %zu bytes in use\n", fj_alloc_used());
		fails++;
	}
	void *big = t_malloc(sizeof(arena) - 64);
	if (!big) {
		printf("arena did not coalesce back into one block\n");
		fails++;
	}
	printf("%s\n", fails ? "FAIL" : "ok");
	return fails != 0;
}
