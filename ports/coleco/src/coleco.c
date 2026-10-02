// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Native module `coleco`: a ColecoVision emulator (cv.h, z80.h) as a
// CircuitPython native .mpy, following the nes module next door.
//
// Every buffer lives in a Python-owned bytearray and is passed in, so the GC
// sees all of it and nothing has to be allocated from native code:
//
//   st = bytearray(coleco.STATE_SIZE)            # machine state, keep alive
//   coleco.init(st, rom)                         # rom padded to 8 KB, keep alive
//   coleco.init(st, rom, bios)                   # or with an 8 KB BIOS image
//   coleco.set_output(st, fb, width, bpp, x0, y0)  # 256x192 at 1x
//   coleco.set_input(st, mask1, mask2)           # 1 = pressed, see below
//   coleco.run_frames(st, n)                     # -> frames run
//   coleco.frame_crc(st)                         # CRC-32 of the last frame
//   coleco.set_audio(st, ring, start)            # optional, like nes
//
// Output formats: bpp 8 is RGB332 (picodvi 8-bit), bpp 16 is RGB565. The
// rest of the framebuffer is filled with the VDP backdrop color.
//
// Input mask bits: UP RIGHT DOWN LEFT FIRE_L FIRE_R and KEY_0..KEY_9,
// KEY_STAR, KEY_HASH for the keypad (if several keys are held the lowest wins).

#ifdef HOST_TEST
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#else
#include "py/dynruntime.h"

// dynruntime.h does not map these; the core and GCC's struct copies need them.
void *memset(void *s, int c, size_t n) {
    return mp_fun_table.memset_(s, c, n);
}
void *memmove(void *dest, const void *src, size_t n) {
    return mp_fun_table.memmove_(dest, src, n);
}
void *memcpy(void *dest, const void *src, size_t n) {
    return mp_fun_table.memmove_(dest, src, n);
}
#endif

#include "cv.h"

// Core entry points, shared by the host harness and the native module.
static int ctx_init(cv_t *m, const uint8_t *rom, uint32_t len, const uint8_t *bios) {
    return cv_init(m, rom, len, bios);
}

static uint32_t ctx_run_frames(cv_t *m, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        cv_run_frame(m);
    }
    return count;
}

#ifndef HOST_TEST

static cv_t *get_ctx(mp_obj_t st_in) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(st_in, &buf, MP_BUFFER_RW);
    if (buf.len < sizeof(cv_t)) {
        mp_raise_ValueError(MP_ERROR_TEXT("state buffer too small"));
    }
    return (cv_t *)buf.buf;
}

// init(st, rom[, bios]) -> 0, or 1 if the ROM size is not a multiple of 8 KB
// (16 KB above 32 KB) or over 1 MB.
static mp_obj_t mod_init(size_t n_args, const mp_obj_t *args) {
    cv_t *m = get_ctx(args[0]);
    mp_buffer_info_t rom;
    mp_get_buffer_raise(args[1], &rom, MP_BUFFER_READ);
    const uint8_t *bios = NULL;
    if (n_args > 2 && args[2] != mp_const_none) {
        mp_buffer_info_t b;
        mp_get_buffer_raise(args[2], &b, MP_BUFFER_READ);
        if (b.len != 8192) {
            mp_raise_ValueError(MP_ERROR_TEXT("BIOS must be 8192 bytes"));
        }
        bios = b.buf;
    }
    return mp_obj_new_int(ctx_init(m, rom.buf, rom.len, bios));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_init_obj, 2, 3, mod_init);

// header_ok(st) -> True if the cartridge starts with AA 55 or 55 AA.
static mp_obj_t mod_header_ok(mp_obj_t st_in) {
    return mp_obj_new_bool(cv_header_ok(get_ctx(st_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_header_ok_obj, mod_header_ok);

// set_output(st, fb, width, bpp, x0, y0); fb=None disables drawing.
static mp_obj_t mod_set_output(size_t n_args, const mp_obj_t *args) {
    cv_t *m = get_ctx(args[0]);
    if (args[1] == mp_const_none) {
        m->fb = NULL;
        return mp_const_none;
    }
    mp_buffer_info_t fb;
    mp_get_buffer_raise(args[1], &fb, MP_BUFFER_WRITE);
    uint32_t width = mp_obj_get_int(args[2]);
    uint32_t bpp = mp_obj_get_int(args[3]);
    uint32_t x0 = mp_obj_get_int(args[4]);
    uint32_t y0 = mp_obj_get_int(args[5]);
    uint32_t bytes = bpp / 8;
    if ((bpp != 8 && bpp != 16) || width == 0 || x0 + CV_W > width ||
        (y0 + CV_H) * width * bytes > fb.len) {
        mp_raise_ValueError(MP_ERROR_TEXT("output does not fit"));
    }
    m->fb = fb.buf;
    m->fb_width = width;
    m->fb_height = fb.len / (width * bytes);
    m->bpp = bpp;
    m->x0 = x0;
    m->y0 = y0;
    m->border = 0xFF;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_set_output_obj, 6, 6, mod_set_output);

// set_audio(st, ring, start): ring is int16 stereo (array('h') / bytearray),
// start is the stereo frame to write next. ring=None stops audio output.
static mp_obj_t mod_set_audio(mp_obj_t st_in, mp_obj_t ring_in, mp_obj_t start_in) {
    cv_t *m = get_ctx(st_in);
    if (ring_in == mp_const_none) {
        m->ring = NULL;
        return mp_const_none;
    }
    mp_buffer_info_t ring;
    mp_get_buffer_raise(ring_in, &ring, MP_BUFFER_WRITE);
    uint32_t frames = ring.len / 4;
    uint32_t start = mp_obj_get_int(start_in);
    if (frames == 0 || start >= frames) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad audio ring"));
    }
    m->ring = ring.buf;
    m->ring_frames = frames;
    m->ring_pos = start;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_set_audio_obj, mod_set_audio);

// set_input(st, mask1[, mask2]) for controllers 1 and 2.
static mp_obj_t mod_set_input(size_t n_args, const mp_obj_t *args) {
    cv_t *m = get_ctx(args[0]);
    m->input[0] = mp_obj_get_int(args[1]);
    if (n_args > 2) {
        m->input[1] = mp_obj_get_int(args[2]);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_set_input_obj, 2, 3, mod_set_input);

static mp_obj_t mod_run_frames(mp_obj_t st_in, mp_obj_t n_in) {
    return mp_obj_new_int(ctx_run_frames(get_ctx(st_in), mp_obj_get_int(n_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_run_frames_obj, mod_run_frames);

// First call turns CRC tracking on (it costs time) and returns 0; later calls
// return the CRC-32 of the last complete frame's 256x192 color indices.
static mp_obj_t mod_frame_crc(mp_obj_t st_in) {
    cv_t *m = get_ctx(st_in);
    m->crc_on = 1;
    return mp_obj_new_int_from_uint(m->last_crc);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_frame_crc_obj, mod_frame_crc);

mp_obj_t mpy_init(mp_obj_fun_bc_t *self, size_t n_args, size_t n_kw, mp_obj_t *args) {
    MP_DYNRUNTIME_INIT_ENTRY
    mp_store_global(MP_QSTR_STATE_SIZE, MP_OBJ_NEW_SMALL_INT(sizeof(cv_t)));
    mp_store_global(MP_QSTR_WIDTH, MP_OBJ_NEW_SMALL_INT(CV_W));
    mp_store_global(MP_QSTR_HEIGHT, MP_OBJ_NEW_SMALL_INT(CV_H));
    mp_store_global(MP_QSTR_AUDIO_RATE, MP_OBJ_NEW_SMALL_INT(CV_AUDIO_RATE));
    mp_store_global(MP_QSTR_AUDIO_SAMPLES_PER_FRAME, MP_OBJ_NEW_SMALL_INT(CV_AUDIO_SAMPLES));
    mp_store_global(MP_QSTR_UP, MP_OBJ_NEW_SMALL_INT(CV_UP));
    mp_store_global(MP_QSTR_RIGHT, MP_OBJ_NEW_SMALL_INT(CV_RIGHT));
    mp_store_global(MP_QSTR_DOWN, MP_OBJ_NEW_SMALL_INT(CV_DOWN));
    mp_store_global(MP_QSTR_LEFT, MP_OBJ_NEW_SMALL_INT(CV_LEFT));
    mp_store_global(MP_QSTR_FIRE_L, MP_OBJ_NEW_SMALL_INT(CV_FIRE_L));
    mp_store_global(MP_QSTR_FIRE_R, MP_OBJ_NEW_SMALL_INT(CV_FIRE_R));
    mp_store_global(MP_QSTR_KEY_0, MP_OBJ_NEW_SMALL_INT(CV_KEY(0)));
    mp_store_global(MP_QSTR_KEY_1, MP_OBJ_NEW_SMALL_INT(CV_KEY(1)));
    mp_store_global(MP_QSTR_KEY_2, MP_OBJ_NEW_SMALL_INT(CV_KEY(2)));
    mp_store_global(MP_QSTR_KEY_3, MP_OBJ_NEW_SMALL_INT(CV_KEY(3)));
    mp_store_global(MP_QSTR_KEY_4, MP_OBJ_NEW_SMALL_INT(CV_KEY(4)));
    mp_store_global(MP_QSTR_KEY_5, MP_OBJ_NEW_SMALL_INT(CV_KEY(5)));
    mp_store_global(MP_QSTR_KEY_6, MP_OBJ_NEW_SMALL_INT(CV_KEY(6)));
    mp_store_global(MP_QSTR_KEY_7, MP_OBJ_NEW_SMALL_INT(CV_KEY(7)));
    mp_store_global(MP_QSTR_KEY_8, MP_OBJ_NEW_SMALL_INT(CV_KEY(8)));
    mp_store_global(MP_QSTR_KEY_9, MP_OBJ_NEW_SMALL_INT(CV_KEY(9)));
    mp_store_global(MP_QSTR_KEY_STAR, MP_OBJ_NEW_SMALL_INT(CV_KEY(10)));
    mp_store_global(MP_QSTR_KEY_HASH, MP_OBJ_NEW_SMALL_INT(CV_KEY(11)));
    mp_store_global(MP_QSTR_init, MP_OBJ_FROM_PTR(&mod_init_obj));
    mp_store_global(MP_QSTR_header_ok, MP_OBJ_FROM_PTR(&mod_header_ok_obj));
    mp_store_global(MP_QSTR_set_output, MP_OBJ_FROM_PTR(&mod_set_output_obj));
    mp_store_global(MP_QSTR_set_input, MP_OBJ_FROM_PTR(&mod_set_input_obj));
    mp_store_global(MP_QSTR_set_audio, MP_OBJ_FROM_PTR(&mod_set_audio_obj));
    mp_store_global(MP_QSTR_run_frames, MP_OBJ_FROM_PTR(&mod_run_frames_obj));
    mp_store_global(MP_QSTR_frame_crc, MP_OBJ_FROM_PTR(&mod_frame_crc_obj));
    MP_DYNRUNTIME_INIT_EXIT
}

#endif
