// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Native module `supervision`: a Watara Supervision emulator (svcore.h) as a
// CircuitPython native .mpy, following the lynx module next door.
//
// Every buffer lives in a Python-owned bytearray and is passed in, so the GC
// sees all of it and nothing has to be allocated from native code:
//
//   st = bytearray(supervision.STATE_SIZE)     # core state incl. WRAM/VRAM
//   supervision.init(st, rom)                  # rom: .sv/.bin bytes, keep alive
//   supervision.info(st)                       # (rom bytes, 16 KB banks)
//   supervision.set_output(st, fb, w, h, bpp, mode)  # centred, see MODE_*
//   supervision.set_palette(st, c0, c1, c2, c3)      # RGB888, lightest first
//   supervision.set_joypad(st, mask)           # 1 = pressed, see JOYPAD_*
//   supervision.run_frames(st, n)              # -> frames run (61.04 Hz)
//   supervision.samples(st)                    # samples made by the last run
//   supervision.frame_crc(st)                  # CRC-32 of the last picture
//   supervision.set_audio(st, ring, start)     # optional, see below
//
// Output formats: bpp 8 is RGB332 (picodvi 8-bit), bpp 16 is RGB565. The
// picture is only redrawn when VRAM, the scroll registers or the palette
// changed.
//
// Sound: stereo samples at AUDIO_RATE follow emulated time, about 361.3 per
// frame (samples() tells). With a ring set they are written into it as int16
// L/R pairs, wrapping. The Supervision's 4-bit DACs only go positive, so
// silence is 0 and full scale is 30000.

#ifdef HOST_TEST
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#else
#include "py/dynruntime.h"

// dynruntime.h does not map these; GCC's struct copies and loops may call them.
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

#include "svcore.h"

// Clear the framebuffer and centre the picture for the mode.
static int ctx_set_output(sv_t *s, uint8_t *fb, uint32_t len, uint32_t w, uint32_t h, uint32_t bpp,
    uint32_t mode) {
    uint32_t pw = mode == SV_MODE_1X ? SV_W : 3 * SV_W / 2;
    uint32_t ph = mode == SV_MODE_1X ? SV_H : 3 * SV_H / 2;
    if ((bpp != 8 && bpp != 16) || mode > SV_MODE_SHARP || w < pw || h < ph || w * h * (bpp / 8) > len) {
        return 1;
    }
    s->fb = fb;
    s->fb_w = (uint16_t)w;
    s->fb_h = (uint16_t)h;
    s->bpp = (uint8_t)bpp;
    s->mode = (uint8_t)mode;
    s->x0 = (uint16_t)((w - pw) / 2);
    s->y0 = (uint16_t)((h - ph) / 2);
    for (uint32_t i = 0; i < w * h * (bpp / 8); i++) {
        fb[i] = 0;
    }
    s->pal_dirty = 1;  // rebuild the LUT for this bpp and redraw at the next frame
    return 0;
}

static uint32_t ctx_run_frames(sv_t *s, uint32_t count) {
    uint32_t f0 = s->frames;
    sv_run(s, count);
    return s->frames - f0;
}

#ifndef HOST_TEST

static sv_t *get_ctx(mp_obj_t st_in) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(st_in, &buf, MP_BUFFER_RW);
    if (buf.len < sizeof(sv_t)) {
        mp_raise_ValueError(MP_ERROR_TEXT("state buffer too small"));
    }
    return (sv_t *)buf.buf;
}

// init(st, rom) -> 0 on success, 1 if rom is not a whole number of 8 KB pages.
static mp_obj_t mod_init(mp_obj_t st_in, mp_obj_t rom_in) {
    sv_t *s = get_ctx(st_in);
    mp_buffer_info_t rom;
    mp_get_buffer_raise(rom_in, &rom, MP_BUFFER_READ);
    return mp_obj_new_int(sv_init(s, rom.buf, rom.len));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_init_obj, mod_init);

// info(st) -> (rom bytes, 16 KB banks)
static mp_obj_t mod_info(mp_obj_t st_in) {
    sv_t *s = get_ctx(st_in);
    mp_obj_t t[2] = {mp_obj_new_int(s->rom_size), mp_obj_new_int(s->banks)};
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_info_obj, mod_info);

// set_output(st, fb, width, height, bpp, mode); fb=None disables drawing.
// Clears the whole framebuffer and centres the picture.
static mp_obj_t mod_set_output(size_t n_args, const mp_obj_t *args) {
    sv_t *s = get_ctx(args[0]);
    if (args[1] == mp_const_none) {
        s->fb = NULL;
        return mp_const_none;
    }
    mp_buffer_info_t fb;
    mp_get_buffer_raise(args[1], &fb, MP_BUFFER_WRITE);
    if (ctx_set_output(s, fb.buf, fb.len, mp_obj_get_int(args[2]), mp_obj_get_int(args[3]),
        mp_obj_get_int(args[4]), mp_obj_get_int(args[5]))) {
        mp_raise_ValueError(MP_ERROR_TEXT("output does not fit"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_set_output_obj, 6, 6, mod_set_output);

// set_palette(st, c0, c1, c2, c3): 0xRRGGBB for shades 0 (lightest) .. 3.
static mp_obj_t mod_set_palette(size_t n_args, const mp_obj_t *args) {
    sv_t *s = get_ctx(args[0]);
    uint32_t c[4];
    for (int i = 0; i < 4; i++) {
        c[i] = mp_obj_get_int(args[1 + i]);
    }
    sv_set_palette(s, c);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_set_palette_obj, 5, 5, mod_set_palette);

// set_audio(st, ring, start): ring is int16 stereo (array('h') / bytearray),
// start is the stereo frame to write next. ring=None stops audio output.
static mp_obj_t mod_set_audio(mp_obj_t st_in, mp_obj_t ring_in, mp_obj_t start_in) {
    sv_t *s = get_ctx(st_in);
    if (ring_in == mp_const_none) {
        s->ring = NULL;
        return mp_const_none;
    }
    mp_buffer_info_t ring;
    mp_get_buffer_raise(ring_in, &ring, MP_BUFFER_WRITE);
    uint32_t frames = ring.len / 4;
    uint32_t start = mp_obj_get_int(start_in);
    if (frames == 0 || start >= frames) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad audio ring"));
    }
    s->ring = ring.buf;
    s->ring_frames = frames;
    s->ring_pos = start;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_set_audio_obj, mod_set_audio);

static mp_obj_t mod_set_joypad(mp_obj_t st_in, mp_obj_t mask_in) {
    get_ctx(st_in)->joy = (uint8_t)mp_obj_get_int(mask_in);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_set_joypad_obj, mod_set_joypad);

static mp_obj_t mod_run_frames(mp_obj_t st_in, mp_obj_t n_in) {
    return mp_obj_new_int(ctx_run_frames(get_ctx(st_in), mp_obj_get_int(n_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_run_frames_obj, mod_run_frames);

static mp_obj_t mod_samples(mp_obj_t st_in) {
    return mp_obj_new_int(get_ctx(st_in)->samples);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_samples_obj, mod_samples);

// First call turns CRC tracking on and returns 0; later calls return the
// CRC-32 of the last drawn picture (160 lines of 40 bytes, 2 bpp).
static mp_obj_t mod_frame_crc(mp_obj_t st_in) {
    sv_t *s = get_ctx(st_in);
    s->crc_on = 1;
    return mp_obj_new_int_from_uint(s->last_crc);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_frame_crc_obj, mod_frame_crc);

// error(st) -> (code, pc); code ERR_STP means the CPU executed STP.
static mp_obj_t mod_error(mp_obj_t st_in) {
    sv_t *s = get_ctx(st_in);
    mp_obj_t t[2] = {mp_obj_new_int(s->err), mp_obj_new_int(s->PC)};
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_error_obj, mod_error);

mp_obj_t mpy_init(mp_obj_fun_bc_t *self, size_t n_args, size_t n_kw, mp_obj_t *args) {
    MP_DYNRUNTIME_INIT_ENTRY
    mp_store_global(MP_QSTR_STATE_SIZE, MP_OBJ_NEW_SMALL_INT(sizeof(sv_t)));
    mp_store_global(MP_QSTR_WIDTH, MP_OBJ_NEW_SMALL_INT(SV_W));
    mp_store_global(MP_QSTR_HEIGHT, MP_OBJ_NEW_SMALL_INT(SV_H));
    mp_store_global(MP_QSTR_AUDIO_RATE, MP_OBJ_NEW_SMALL_INT(SV_AUDIO_RATE));
    mp_store_global(MP_QSTR_MODE_SMOOTH, MP_OBJ_NEW_SMALL_INT(SV_MODE_SMOOTH));
    mp_store_global(MP_QSTR_MODE_1X, MP_OBJ_NEW_SMALL_INT(SV_MODE_1X));
    mp_store_global(MP_QSTR_MODE_SHARP, MP_OBJ_NEW_SMALL_INT(SV_MODE_SHARP));
    mp_store_global(MP_QSTR_ERR_STP, MP_OBJ_NEW_SMALL_INT(SV_ERR_STP));
    mp_store_global(MP_QSTR_JOYPAD_RIGHT, MP_OBJ_NEW_SMALL_INT(SV_RIGHT));
    mp_store_global(MP_QSTR_JOYPAD_LEFT, MP_OBJ_NEW_SMALL_INT(SV_LEFT));
    mp_store_global(MP_QSTR_JOYPAD_DOWN, MP_OBJ_NEW_SMALL_INT(SV_DOWN));
    mp_store_global(MP_QSTR_JOYPAD_UP, MP_OBJ_NEW_SMALL_INT(SV_UP));
    mp_store_global(MP_QSTR_JOYPAD_B, MP_OBJ_NEW_SMALL_INT(SV_B));
    mp_store_global(MP_QSTR_JOYPAD_A, MP_OBJ_NEW_SMALL_INT(SV_A));
    mp_store_global(MP_QSTR_JOYPAD_SELECT, MP_OBJ_NEW_SMALL_INT(SV_SELECT));
    mp_store_global(MP_QSTR_JOYPAD_START, MP_OBJ_NEW_SMALL_INT(SV_START));
    mp_store_global(MP_QSTR_init, MP_OBJ_FROM_PTR(&mod_init_obj));
    mp_store_global(MP_QSTR_info, MP_OBJ_FROM_PTR(&mod_info_obj));
    mp_store_global(MP_QSTR_set_output, MP_OBJ_FROM_PTR(&mod_set_output_obj));
    mp_store_global(MP_QSTR_set_palette, MP_OBJ_FROM_PTR(&mod_set_palette_obj));
    mp_store_global(MP_QSTR_set_joypad, MP_OBJ_FROM_PTR(&mod_set_joypad_obj));
    mp_store_global(MP_QSTR_set_audio, MP_OBJ_FROM_PTR(&mod_set_audio_obj));
    mp_store_global(MP_QSTR_run_frames, MP_OBJ_FROM_PTR(&mod_run_frames_obj));
    mp_store_global(MP_QSTR_samples, MP_OBJ_FROM_PTR(&mod_samples_obj));
    mp_store_global(MP_QSTR_frame_crc, MP_OBJ_FROM_PTR(&mod_frame_crc_obj));
    mp_store_global(MP_QSTR_error, MP_OBJ_FROM_PTR(&mod_error_obj));
    MP_DYNRUNTIME_INIT_EXIT
}

#endif
