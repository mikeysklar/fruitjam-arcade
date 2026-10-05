/* fruitjam-scumm - SCUMM v3/v4 on the Adafruit Fruit Jam
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * C++ runtime support for the natmod build: new/delete on the arena
 * allocator, and the pure virtual trap. No exceptions, no RTTI.
 *
 * The host harness uses the arena too (fj_host_arena.h), but new/delete
 * there are global, so allocations made before the arena exists, and
 * frees of them, go to the system allocator.
 */

#include <stdlib.h>
#include <stddef.h>

#include "backend/fj_alloc.h"

#ifdef FJ_HOST
#undef malloc
#undef free
#include <new>

static void *fjNew(size_t n) {
	if (fj_alloc_ready())
		return fj_host_malloc(n ? n : 1);
	return malloc(n ? n : 1);
}

static void fjDelete(void *p) {
	if (fj_alloc_owns(p))
		fj_host_free(p);
	else
		free(p);
}

void *operator new(size_t n) {
	return fjNew(n);
}

void *operator new[](size_t n) {
	return fjNew(n);
}

void operator delete(void *p) noexcept {
	fjDelete(p);
}

void operator delete[](void *p) noexcept {
	fjDelete(p);
}

void operator delete(void *p, size_t) noexcept {
	fjDelete(p);
}

void operator delete[](void *p, size_t) noexcept {
	fjDelete(p);
}
#endif

#ifdef FJ_NATMOD

void *operator new(size_t n) {
	return malloc(n);
}

void *operator new[](size_t n) {
	return malloc(n);
}

void operator delete(void *p) {
	free(p);
}

void operator delete[](void *p) {
	free(p);
}

void operator delete(void *p, size_t) {
	free(p);
}

void operator delete[](void *p, size_t) {
	free(p);
}

void error(const char *s, ...);

extern "C" void __cxa_pure_virtual() {
	error("pure virtual call");
}

void *__dso_handle;

#endif
