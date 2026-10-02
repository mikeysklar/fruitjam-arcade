// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Native module `lynx`: an Atari Lynx emulator (lynxcore.h) as a
// CircuitPython native .mpy, following the nes module next door.
//
// Every buffer lives in a Python-owned bytearray and is passed in, so the GC
// sees all of it and nothing has to be allocated from native code:
//
//   st = bytearray(lynx.STATE_SIZE)          # core state incl. 64 KB RAM
//   lynx.init(st, rom, boot)                 # rom: .lnx/.o bytes, keep alive;
//                                            # boot: lynxboot.img bytes or None
//   lynx.info(st)                            # (boot, rotation, addr, size, page)
//   lynx.set_output(st, fb, width, bpp, x0, y0)  # 160x102 drawn 2x = 320x204
//   lynx.set_joypad(st, mask)                # 1 = pressed, see JOYPAD_* below
//   lynx.run_frames(st, n)                   # -> frames run
//   lynx.samples(st)                         # samples made by the last run
//   lynx.frame_crc(st)                       # CRC-32 of the last frame
//   lynx.set_audio(st, ring, start)          # optional, see below
//
// Output formats: bpp 8 is RGB332 (picodvi 8-bit), bpp 16 is RGB565.
//
// Sound: stereo samples at AUDIO_RATE follow emulated time, so their number
// per frame depends on the game's frame rate (samples() tells). With a ring
// set they are written into it as int16 L/R pairs, wrapping. Make the ring
// the buffer of a looping, double buffered audiocore.RawSample and start
// writing one half ahead of playback.

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

#include "lynxcore.h"

// Longest a run_frames(n) call may take in emulated time per frame asked for,
// in case the game stops its display timers: 1/30 s.
#define MAX_TICKS_PER_FRAME (LYNX_TICKS_PER_SEC / 30)

static uint32_t ctx_run_frames(lynx_t *l, uint32_t count) {
    uint32_t f0 = l->frames;
    lynx_run(l, count, count * MAX_TICKS_PER_FRAME);
    return l->frames - f0;
}

#ifndef HOST_TEST

static lynx_t *get_ctx(mp_obj_t st_in) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(st_in, &buf, MP_BUFFER_RW);
    if (buf.len < sizeof(lynx_t)) {
        mp_raise_ValueError(MP_ERROR_TEXT("state buffer too small"));
    }
    return (lynx_t *)buf.buf;
}

// init(st, rom, boot) -> 0 on success; boot is lynxboot.img (512 bytes) or None.
static mp_obj_t mod_init(mp_obj_t st_in, mp_obj_t rom_in, mp_obj_t boot_in) {
    lynx_t *l = get_ctx(st_in);
    mp_buffer_info_t rom;
    mp_get_buffer_raise(rom_in, &rom, MP_BUFFER_READ);
    const uint8_t *boot = NULL;
    if (boot_in != mp_const_none) {
        mp_buffer_info_t b;
        mp_get_buffer_raise(boot_in, &b, MP_BUFFER_READ);
        if (b.len < 512) {
            mp_raise_ValueError(MP_ERROR_TEXT("boot ROM must be 512 bytes"));
        }
        boot = b.buf;
    }
    return mp_obj_new_int(lynx_init(l, rom.buf, rom.len, boot));
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_init_obj, mod_init);

// info(st) -> (boot, rotation, addr, size, page)
//   boot: BOOT_HLE (.lnx decrypted by the built in loader), BOOT_BS93 (.o
//   homebrew loaded straight to RAM) or BOOT_ROM (real lynxboot.img).
//   HLE: addr is where the first loader went, size its block count.
//   BS93: addr is the load address, size the bytes loaded.
//   rotation: .lnx header byte, 0 none, 1 left, 2 right. page: bank 0 page size.
static mp_obj_t mod_info(mp_obj_t st_in) {
    lynx_t *l = get_ctx(st_in);
    mp_obj_t t[5] = {
        mp_obj_new_int(l->boot), mp_obj_new_int(l->rotation), mp_obj_new_int(l->boot_addr),
        mp_obj_new_int(l->boot_blocks), mp_obj_new_int(l->bank0_shift ? 1 << l->bank0_shift : 0),
    };
    return mp_obj_new_tuple(5, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_info_obj, mod_info);

// set_output(st, fb, width, bpp, x0, y0); fb=None disables drawing.
static mp_obj_t mod_set_output(size_t n_args, const mp_obj_t *args) {
    lynx_t *l = get_ctx(args[0]);
    if (args[1] == mp_const_none) {
        l->fb = NULL;
        return mp_const_none;
    }
    mp_buffer_info_t fb;
    mp_get_buffer_raise(args[1], &fb, MP_BUFFER_WRITE);
    uint32_t width = mp_obj_get_int(args[2]);
    uint32_t bpp = mp_obj_get_int(args[3]);
    uint32_t x0 = mp_obj_get_int(args[4]);
    uint32_t y0 = mp_obj_get_int(args[5]);
    uint32_t bytes = bpp / 8;
    if ((bpp != 8 && bpp != 16) || (x0 & 1) || (width & 1) || x0 + 2 * LYNX_W > width ||
        (y0 + 2 * LYNX_H) * width * bytes > fb.len) {
        mp_raise_ValueError(MP_ERROR_TEXT("output does not fit"));
    }
    l->fb = fb.buf;
    l->fb_width = width;
    l->bpp = bpp;
    l->x0 = x0;
    l->y0 = y0;
    l->pal_dirty = 1;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_set_output_obj, 6, 6, mod_set_output);

// set_audio(st, ring, start): ring is int16 stereo (array('h') / bytearray),
// start is the stereo frame to write next. ring=None stops audio output.
static mp_obj_t mod_set_audio(mp_obj_t st_in, mp_obj_t ring_in, mp_obj_t start_in) {
    lynx_t *l = get_ctx(st_in);
    if (ring_in == mp_const_none) {
        l->ring = NULL;
        return mp_const_none;
    }
    mp_buffer_info_t ring;
    mp_get_buffer_raise(ring_in, &ring, MP_BUFFER_WRITE);
    uint32_t frames = ring.len / 4;
    uint32_t start = mp_obj_get_int(start_in);
    if (frames == 0 || start >= frames) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad audio ring"));
    }
    l->ring = ring.buf;
    l->ring_frames = frames;
    l->ring_pos = start;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_set_audio_obj, mod_set_audio);

static mp_obj_t mod_set_joypad(mp_obj_t st_in, mp_obj_t mask_in) {
    get_ctx(st_in)->joy = (uint16_t)mp_obj_get_int(mask_in);
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

// First call turns CRC tracking on (it costs time) and returns 0; later calls
// return the CRC-32 of the last complete frame's 12-bit palette colors.
static mp_obj_t mod_frame_crc(mp_obj_t st_in) {
    lynx_t *l = get_ctx(st_in);
    l->crc_on = 1;
    return mp_obj_new_int_from_uint(l->last_crc);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_frame_crc_obj, mod_frame_crc);

// error(st) -> (code, pc); code 3 means the CPU executed STP.
static mp_obj_t mod_error(mp_obj_t st_in) {
    lynx_t *l = get_ctx(st_in);
    mp_obj_t t[2] = {mp_obj_new_int(l->err), mp_obj_new_int(l->PC)};
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_error_obj, mod_error);

mp_obj_t mpy_init(mp_obj_fun_bc_t *self, size_t n_args, size_t n_kw, mp_obj_t *args) {
    MP_DYNRUNTIME_INIT_ENTRY
    mp_store_global(MP_QSTR_STATE_SIZE, MP_OBJ_NEW_SMALL_INT(sizeof(lynx_t)));
    mp_store_global(MP_QSTR_WIDTH, MP_OBJ_NEW_SMALL_INT(LYNX_W));
    mp_store_global(MP_QSTR_HEIGHT, MP_OBJ_NEW_SMALL_INT(LYNX_H));
    mp_store_global(MP_QSTR_AUDIO_RATE, MP_OBJ_NEW_SMALL_INT(LYNX_AUDIO_RATE));
    mp_store_global(MP_QSTR_BOOT_HLE, MP_OBJ_NEW_SMALL_INT(BOOT_HLE));
    mp_store_global(MP_QSTR_BOOT_BS93, MP_OBJ_NEW_SMALL_INT(BOOT_BS93));
    mp_store_global(MP_QSTR_BOOT_ROM, MP_OBJ_NEW_SMALL_INT(BOOT_ROM));
    mp_store_global(MP_QSTR_JOYPAD_A, MP_OBJ_NEW_SMALL_INT(LYNX_A));
    mp_store_global(MP_QSTR_JOYPAD_B, MP_OBJ_NEW_SMALL_INT(LYNX_B));
    mp_store_global(MP_QSTR_JOYPAD_OPT1, MP_OBJ_NEW_SMALL_INT(LYNX_OPT1));
    mp_store_global(MP_QSTR_JOYPAD_OPT2, MP_OBJ_NEW_SMALL_INT(LYNX_OPT2));
    mp_store_global(MP_QSTR_JOYPAD_PAUSE, MP_OBJ_NEW_SMALL_INT(LYNX_PAUSE));
    mp_store_global(MP_QSTR_JOYPAD_UP, MP_OBJ_NEW_SMALL_INT(LYNX_UP));
    mp_store_global(MP_QSTR_JOYPAD_DOWN, MP_OBJ_NEW_SMALL_INT(LYNX_DOWN));
    mp_store_global(MP_QSTR_JOYPAD_LEFT, MP_OBJ_NEW_SMALL_INT(LYNX_LEFT));
    mp_store_global(MP_QSTR_JOYPAD_RIGHT, MP_OBJ_NEW_SMALL_INT(LYNX_RIGHT));
    mp_store_global(MP_QSTR_init, MP_OBJ_FROM_PTR(&mod_init_obj));
    mp_store_global(MP_QSTR_info, MP_OBJ_FROM_PTR(&mod_info_obj));
    mp_store_global(MP_QSTR_set_output, MP_OBJ_FROM_PTR(&mod_set_output_obj));
    mp_store_global(MP_QSTR_set_joypad, MP_OBJ_FROM_PTR(&mod_set_joypad_obj));
    mp_store_global(MP_QSTR_set_audio, MP_OBJ_FROM_PTR(&mod_set_audio_obj));
    mp_store_global(MP_QSTR_run_frames, MP_OBJ_FROM_PTR(&mod_run_frames_obj));
    mp_store_global(MP_QSTR_samples, MP_OBJ_FROM_PTR(&mod_samples_obj));
    mp_store_global(MP_QSTR_frame_crc, MP_OBJ_FROM_PTR(&mod_frame_crc_obj));
    mp_store_global(MP_QSTR_error, MP_OBJ_FROM_PTR(&mod_error_obj));
    MP_DYNRUNTIME_INIT_EXIT
}

#endif
