// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Native module `msx`: an MSX1 emulator (msx.h, z80.h) as a CircuitPython
// native .mpy, following the coleco module next door.
//
// Every buffer lives in a Python-owned bytearray and is passed in, so the GC
// sees all of it and nothing has to be allocated from native code:
//
//   st = bytearray(msx.STATE_SIZE)               # machine state incl. 64 KB RAM
//   msx.init(st, rom)                            # rom padded to 8 KB, keep alive
//   msx.init(st, rom, bios, mapper)              # 32 KB BIOS or None, mapper or -1
//   msx.info(st)                                 # -> (mapper, plain ROM base)
//   msx.set_output(st, fb, width, bpp, x0, y0)   # 256x192 at 1x
//   msx.set_input(st, joy1[, joy2])              # UP DOWN LEFT RIGHT TRIG_A TRIG_B
//   msx.set_keys(st, matrix)                     # 11 bytes, keyboard rows, 1 = down
//   msx.run_frames(st, n)                        # -> frames run
//   msx.frame_crc(st)                            # CRC-32 of the last frame
//   msx.set_audio(st, ring, start)               # optional, like coleco
//
// With bios None the built-in C-BIOS 0.29a (BSD-style license, see
// cbios/LICENSE.txt) is used, with its logo ROM.
//
// Output formats: bpp 8 is RGB332 (picodvi 8-bit), bpp 16 is RGB565. The
// rest of the framebuffer is filled with the VDP backdrop color.

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

#include "msx.h"

// Core entry points, shared by the host harness and the native module.
static int ctx_init(msx_t *m, const uint8_t *rom, uint32_t len, const uint8_t *bios, int mapper) {
    return msx_init(m, rom, len, bios, mapper);
}

static uint32_t ctx_run_frames(msx_t *m, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        msx_run_frame(m);
    }
    return count;
}

#ifndef HOST_TEST

static msx_t *get_ctx(mp_obj_t st_in) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(st_in, &buf, MP_BUFFER_RW);
    if (buf.len < sizeof(msx_t)) {
        mp_raise_ValueError(MP_ERROR_TEXT("state buffer too small"));
    }
    return (msx_t *)buf.buf;
}

// init(st, rom[, bios[, mapper]]) -> 0, 1 if the ROM size is not a multiple
// of 8 KB or over 8 MB, 2 if the mapper is unknown (or plain over 64 KB).
// bios None = built-in C-BIOS; mapper -1 (default) = guess.
static mp_obj_t mod_init(size_t n_args, const mp_obj_t *args) {
    msx_t *m = get_ctx(args[0]);
    mp_buffer_info_t rom;
    mp_get_buffer_raise(args[1], &rom, MP_BUFFER_READ);
    const uint8_t *bios = NULL;
    if (n_args > 2 && args[2] != mp_const_none) {
        mp_buffer_info_t b;
        mp_get_buffer_raise(args[2], &b, MP_BUFFER_READ);
        if (b.len != 32768) {
            mp_raise_ValueError(MP_ERROR_TEXT("BIOS must be 32768 bytes"));
        }
        bios = b.buf;
    }
    int mapper = n_args > 3 ? mp_obj_get_int(args[3]) : -1;
    return mp_obj_new_int(ctx_init(m, rom.buf, rom.len, bios, mapper));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_init_obj, 2, 4, mod_init);

// info(st) -> (mapper, base): base is where a plain ROM starts ($0000, $4000, $8000).
static mp_obj_t mod_info(mp_obj_t st_in) {
    msx_t *m = get_ctx(st_in);
    mp_obj_t t[2] = {mp_obj_new_int(m->mapper), mp_obj_new_int(m->rom_base)};
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_info_obj, mod_info);

// set_output(st, fb, width, bpp, x0, y0); fb=None disables drawing.
static mp_obj_t mod_set_output(size_t n_args, const mp_obj_t *args) {
    msx_t *m = get_ctx(args[0]);
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
    if ((bpp != 8 && bpp != 16) || width == 0 || x0 + MSX_W > width ||
        (y0 + MSX_H) * width * bytes > fb.len) {
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
    msx_t *m = get_ctx(st_in);
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

// set_input(st, joy1[, joy2]) for joystick ports 1 and 2.
static mp_obj_t mod_set_input(size_t n_args, const mp_obj_t *args) {
    msx_t *m = get_ctx(args[0]);
    m->joy[0] = mp_obj_get_int(args[1]);
    if (n_args > 2) {
        m->joy[1] = mp_obj_get_int(args[2]);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_set_input_obj, 2, 3, mod_set_input);

// set_keys(st, matrix): keyboard rows 0-10, bit set = key down. Shorter
// buffers leave the remaining rows up.
static mp_obj_t mod_set_keys(mp_obj_t st_in, mp_obj_t keys_in) {
    msx_t *m = get_ctx(st_in);
    mp_buffer_info_t k;
    mp_get_buffer_raise(keys_in, &k, MP_BUFFER_READ);
    const uint8_t *src = k.buf;
    for (uint32_t i = 0; i < MSX_KBD_ROWS; i++) {
        m->kbd[i] = i < k.len ? src[i] : 0;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_set_keys_obj, mod_set_keys);

static mp_obj_t mod_run_frames(mp_obj_t st_in, mp_obj_t n_in) {
    return mp_obj_new_int(ctx_run_frames(get_ctx(st_in), mp_obj_get_int(n_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_run_frames_obj, mod_run_frames);

// First call turns CRC tracking on (it costs time) and returns 0; later calls
// return the CRC-32 of the last complete frame's 256x192 color indices.
static mp_obj_t mod_frame_crc(mp_obj_t st_in) {
    msx_t *m = get_ctx(st_in);
    m->crc_on = 1;
    return mp_obj_new_int_from_uint(m->last_crc);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_frame_crc_obj, mod_frame_crc);

mp_obj_t mpy_init(mp_obj_fun_bc_t *self, size_t n_args, size_t n_kw, mp_obj_t *args) {
    MP_DYNRUNTIME_INIT_ENTRY
    mp_store_global(MP_QSTR_STATE_SIZE, MP_OBJ_NEW_SMALL_INT(sizeof(msx_t)));
    mp_store_global(MP_QSTR_WIDTH, MP_OBJ_NEW_SMALL_INT(MSX_W));
    mp_store_global(MP_QSTR_HEIGHT, MP_OBJ_NEW_SMALL_INT(MSX_H));
    mp_store_global(MP_QSTR_AUDIO_RATE, MP_OBJ_NEW_SMALL_INT(MSX_AUDIO_RATE));
    mp_store_global(MP_QSTR_AUDIO_SAMPLES_PER_FRAME, MP_OBJ_NEW_SMALL_INT(MSX_AUDIO_SAMPLES));
    mp_store_global(MP_QSTR_UP, MP_OBJ_NEW_SMALL_INT(MSX_UP));
    mp_store_global(MP_QSTR_DOWN, MP_OBJ_NEW_SMALL_INT(MSX_DOWN));
    mp_store_global(MP_QSTR_LEFT, MP_OBJ_NEW_SMALL_INT(MSX_LEFT));
    mp_store_global(MP_QSTR_RIGHT, MP_OBJ_NEW_SMALL_INT(MSX_RIGHT));
    mp_store_global(MP_QSTR_TRIG_A, MP_OBJ_NEW_SMALL_INT(MSX_TRIG_A));
    mp_store_global(MP_QSTR_TRIG_B, MP_OBJ_NEW_SMALL_INT(MSX_TRIG_B));
    mp_store_global(MP_QSTR_MAPPER_PLAIN, MP_OBJ_NEW_SMALL_INT(MSX_MAP_PLAIN));
    mp_store_global(MP_QSTR_MAPPER_KONAMI, MP_OBJ_NEW_SMALL_INT(MSX_MAP_KONAMI));
    mp_store_global(MP_QSTR_MAPPER_KONAMI_SCC, MP_OBJ_NEW_SMALL_INT(MSX_MAP_KONAMI_SCC));
    mp_store_global(MP_QSTR_MAPPER_ASCII8, MP_OBJ_NEW_SMALL_INT(MSX_MAP_ASCII8));
    mp_store_global(MP_QSTR_MAPPER_ASCII16, MP_OBJ_NEW_SMALL_INT(MSX_MAP_ASCII16));
    mp_store_global(MP_QSTR_init, MP_OBJ_FROM_PTR(&mod_init_obj));
    mp_store_global(MP_QSTR_info, MP_OBJ_FROM_PTR(&mod_info_obj));
    mp_store_global(MP_QSTR_set_output, MP_OBJ_FROM_PTR(&mod_set_output_obj));
    mp_store_global(MP_QSTR_set_input, MP_OBJ_FROM_PTR(&mod_set_input_obj));
    mp_store_global(MP_QSTR_set_keys, MP_OBJ_FROM_PTR(&mod_set_keys_obj));
    mp_store_global(MP_QSTR_set_audio, MP_OBJ_FROM_PTR(&mod_set_audio_obj));
    mp_store_global(MP_QSTR_run_frames, MP_OBJ_FROM_PTR(&mod_run_frames_obj));
    mp_store_global(MP_QSTR_frame_crc, MP_OBJ_FROM_PTR(&mod_frame_crc_obj));
    MP_DYNRUNTIME_INIT_EXIT
}

#endif
