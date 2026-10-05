/* fruitjam-scumm - SCUMM v3/v4 on the Adafruit Fruit Jam
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Bare-metal driver for running the ARM build of the core under an
 * emulator (tools/armemu/run.py, Unicorn). It links the same objects that
 * go into scumm.mpy. File I/O and the frame loop call "host calls":
 * addresses the emulator hooks, where it does the work and returns.
 */

#include <stddef.h>
#include <stdint.h>

#include "backend/fj_core.h"

#define HOSTCALL(n) (0x0F000001u + (n) * 4)

typedef int (*open_fn)(void *, const char *, int);
typedef int (*rw_fn)(void *, int, void *, int);
typedef int (*wr_fn)(void *, int, const void *, int);
typedef int (*seek_fn)(void *, int, int32_t);
typedef int32_t (*size_fn)(void *, int);
typedef void (*close_fn)(void *, int);
typedef void (*log_fn)(void *, const char *);
typedef void (*frame_fn)(int alive, int init_err);

/* Filled in by the emulator before each frame. */
struct armtest_ctl {
	int32_t x, y, buttons, key, ascii, save, load;
};
volatile struct armtest_ctl armtest_ctl;

uint8_t armtest_arena[2 * 1024 * 1024] __attribute__((aligned(8)));
uint16_t armtest_fb[320 * 240];
int16_t armtest_ring[4096 * 2];

static const fj_io io = {
	(open_fn)HOSTCALL(0), (rw_fn)HOSTCALL(1), (wr_fn)HOSTCALL(2), (seek_fn)HOSTCALL(3),
	(size_fn)HOSTCALL(4), (close_fn)HOSTCALL(5), (log_fn)HOSTCALL(6), 0
};

void *memcpy(void *d, const void *s, size_t n) {
	uint8_t *p = d;
	const uint8_t *q = s;
	while (n--)
		*p++ = *q++;
	return d;
}

void *memmove(void *d, const void *s, size_t n) {
	uint8_t *p = d;
	const uint8_t *q = s;
	if (p < q) {
		while (n--)
			*p++ = *q++;
	} else {
		while (n--)
			p[n] = q[n];
	}
	return d;
}

void *memset(void *d, int c, size_t n) {
	uint8_t *p = d;
	while (n--)
		*p++ = (uint8_t)c;
	return d;
}

void armtest_main(void) {
	frame_fn frame = (frame_fn)HOSTCALL(7);
	int err = fj_core_init(armtest_arena, sizeof(armtest_arena), &io);
	if (err) {
		frame(0, err);
		for (;;) {
		}
	}
	fj_core_set_output(armtest_fb, 320, 240, 16, 0, 20);
	fj_core_set_audio(armtest_ring, 4096, 0);
	int alive = 1;
	for (;;) {
		frame(alive, 0);
		if (armtest_ctl.key) {
			fj_core_key(armtest_ctl.key, armtest_ctl.ascii);
			armtest_ctl.key = 0;
		}
		if (armtest_ctl.save) {
			fj_core_save(armtest_ctl.save);
			armtest_ctl.save = 0;
		}
		if (armtest_ctl.load) {
			fj_core_load(armtest_ctl.load);
			armtest_ctl.load = 0;
		}
		fj_core_input(armtest_ctl.x, armtest_ctl.y, armtest_ctl.buttons);
		alive = fj_core_frame();
	}
}
