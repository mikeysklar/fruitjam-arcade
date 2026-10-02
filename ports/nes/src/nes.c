// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Native module `nes`: an NES emulator (nescore.h) as a CircuitPython native
// .mpy, following the peanutgb module in the parent directory.
//
// Every buffer lives in a Python-owned bytearray and is passed in, so the GC
// sees all of it and nothing has to be allocated from native code:
//
//   st = bytearray(nes.STATE_SIZE)               # core + context, keep alive
//   nes.init(st, rom)                            # rom: bytes/bytearray, keep alive
//   nes.set_cart_ram(st, bytearray(nes.save_size(st)))  # battery carts only
//   nes.set_output(st, fb, width, bpp, x0, y0)   # optional, 256x240 at 1x
//   nes.set_joypad(st, mask)                     # 1 = pressed, see JOYPAD_* below
//   nes.run_frames(st, n)                        # -> frames run, stops on CPU jam
//   nes.frame_crc(st)                            # CRC-32 of the last frame's colors
//   nes.set_audio(st, ring, start)               # optional, see below
//
// Output formats: bpp 8 is RGB332 (picodvi 8-bit), bpp 16 is RGB565.
//
// Sound: the core makes AUDIO_SAMPLES_PER_FRAME mono samples per frame at
// AUDIO_RATE. With a ring set they are appended to it as stereo int16 frames,
// wrapping. Make the ring the buffer of a looping, double buffered
// audiocore.RawSample and start writing one half ahead of playback.

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

#include "nescore.h"

typedef struct {
    nes_t nes;  // first, so nes_t * and ctx_t * are the same pointer
    uint8_t *fb;
    uint32_t fb_width;
    uint32_t bpp;
    uint32_t x0;
    uint32_t y0;
    int16_t *ring;
    uint32_t ring_frames;
    uint32_t ring_pos;
    uint32_t crc_on;
    uint32_t crc;
    uint32_t last_crc;
    uint32_t lut_ver;  // colors_ver the LUTs were built from, +1
    uint16_t lut16[32];
    uint8_t lut8[32];
} ctx_t;

static const uint32_t crc_nibble[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t len) {
    while (len--) {
        c ^= *p++;
        c = (c >> 4) ^ crc_nibble[c & 15];
        c = (c >> 4) ^ crc_nibble[c & 15];
    }
    return c;
}

static void nes_draw_line(nes_t *n, int line, const uint8_t *slots, const uint8_t *colors) {
    ctx_t *ctx = (ctx_t *)n;
    if (ctx->crc_on) {
        uint8_t px[NES_W];
        for (int x = 0; x < NES_W; x++) {
            px[x] = colors[slots[x]];
        }
        if (line == 0) {
            ctx->crc = 0xFFFFFFFF;
        }
        ctx->crc = crc32_update(ctx->crc, px, NES_W);
        if (line == NES_H - 1) {
            ctx->last_crc = ~ctx->crc;
        }
    }
    if (ctx->fb == NULL) {
        return;
    }
    if (ctx->lut_ver != n->colors_ver + 1) {
        for (int i = 0; i < 32; i++) {
            ctx->lut16[i] = nes_pal565[colors[i]];
            ctx->lut8[i] = nes_pal332[colors[i]];
        }
        ctx->lut_ver = n->colors_ver + 1;
    }
    uint32_t off = (ctx->y0 + line) * ctx->fb_width + ctx->x0;
    if (ctx->bpp == 8) {
        const uint8_t *lut = ctx->lut8;
        uint8_t *dst = ctx->fb + off;
        for (int x = 0; x < NES_W; x++) {
            dst[x] = lut[slots[x]];
        }
    } else {
        const uint16_t *lut = ctx->lut16;
        uint16_t *dst = (uint16_t *)ctx->fb + off;
        for (int x = 0; x < NES_W; x++) {
            dst[x] = lut[slots[x]];
        }
    }
}

// Core entry points, shared by the host harness and the native module.
static int ctx_init(ctx_t *ctx, const uint8_t *rom, uint32_t len) {
    memset(ctx, 0, sizeof(*ctx));
    return nes_init(&ctx->nes, rom, len);
}

static uint32_t ctx_run_frames(ctx_t *ctx, uint32_t count) {
    uint32_t i = 0;
    while (i < count && !ctx->nes.err) {
        nes_run_frame(&ctx->nes);
        if (ctx->ring != NULL) {
            // Mono samples duplicated to stereo, copied into the ring with wrap.
            const int16_t *src = ctx->nes.audio;
            int16_t *ring = ctx->ring;
            uint32_t pos = ctx->ring_pos;
            for (uint32_t k = 0; k < NES_AUDIO_SAMPLES; k++) {
                ring[pos * 2] = src[k];
                ring[pos * 2 + 1] = src[k];
                if (++pos == ctx->ring_frames) {
                    pos = 0;
                }
            }
            ctx->ring_pos = pos;
        }
        i++;
    }
    return i;
}

#ifndef HOST_TEST

static ctx_t *get_ctx(mp_obj_t st_in) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(st_in, &buf, MP_BUFFER_RW);
    if (buf.len < sizeof(ctx_t)) {
        mp_raise_ValueError(MP_ERROR_TEXT("state buffer too small"));
    }
    return (ctx_t *)buf.buf;
}

static mp_obj_t mod_init(mp_obj_t st_in, mp_obj_t rom_in) {
    ctx_t *ctx = get_ctx(st_in);
    mp_buffer_info_t rom;
    mp_get_buffer_raise(rom_in, &rom, MP_BUFFER_READ);
    return mp_obj_new_int(ctx_init(ctx, rom.buf, rom.len));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_init_obj, mod_init);

// info(st) -> (mapper, prg_bytes, chr_bytes, battery)
static mp_obj_t mod_info(mp_obj_t st_in) {
    nes_t *n = &get_ctx(st_in)->nes;
    mp_obj_t t[4] = {
        mp_obj_new_int(n->mapper), mp_obj_new_int(n->prg_size),
        mp_obj_new_int(n->chr_is_ram ? 0 : n->chr_size), mp_obj_new_int(n->battery),
    };
    return mp_obj_new_tuple(4, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_info_obj, mod_info);

// Battery-backed PRG RAM size: 8192 for battery carts, else 0.
static mp_obj_t mod_save_size(mp_obj_t st_in) {
    return mp_obj_new_int(get_ctx(st_in)->nes.battery ? 0x2000 : 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_save_size_obj, mod_save_size);

// Use the caller's buffer (>= 8192 bytes) as PRG RAM at $6000, so it can be
// saved. Its current contents are kept, so load the .sav into it first.
static mp_obj_t mod_set_cart_ram(mp_obj_t st_in, mp_obj_t ram_in) {
    ctx_t *ctx = get_ctx(st_in);
    mp_buffer_info_t ram;
    mp_get_buffer_raise(ram_in, &ram, MP_BUFFER_RW);
    if (ram.len < 0x2000) {
        mp_raise_ValueError(MP_ERROR_TEXT("cart RAM must be 8192 bytes"));
    }
    ctx->nes.prg_ram = ram.buf;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_set_cart_ram_obj, mod_set_cart_ram);

// set_output(st, fb, width, bpp, x0, y0); fb=None disables drawing.
static mp_obj_t mod_set_output(size_t n_args, const mp_obj_t *args) {
    ctx_t *ctx = get_ctx(args[0]);
    if (args[1] == mp_const_none) {
        ctx->fb = NULL;
        return mp_const_none;
    }
    mp_buffer_info_t fb;
    mp_get_buffer_raise(args[1], &fb, MP_BUFFER_WRITE);
    uint32_t width = mp_obj_get_int(args[2]);
    uint32_t bpp = mp_obj_get_int(args[3]);
    uint32_t x0 = mp_obj_get_int(args[4]);
    uint32_t y0 = mp_obj_get_int(args[5]);
    uint32_t bytes = bpp / 8;
    if ((bpp != 8 && bpp != 16) || x0 + NES_W > width ||
        (y0 + NES_H) * width * bytes > fb.len) {
        mp_raise_ValueError(MP_ERROR_TEXT("output does not fit"));
    }
    ctx->fb = fb.buf;
    ctx->fb_width = width;
    ctx->bpp = bpp;
    ctx->x0 = x0;
    ctx->y0 = y0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_set_output_obj, 6, 6, mod_set_output);

// set_audio(st, ring, start): ring is int16 stereo (array('h') / bytearray),
// start is the stereo frame to write next. ring=None stops audio output.
static mp_obj_t mod_set_audio(mp_obj_t st_in, mp_obj_t ring_in, mp_obj_t start_in) {
    ctx_t *ctx = get_ctx(st_in);
    if (ring_in == mp_const_none) {
        ctx->ring = NULL;
        return mp_const_none;
    }
    mp_buffer_info_t ring;
    mp_get_buffer_raise(ring_in, &ring, MP_BUFFER_WRITE);
    uint32_t frames = ring.len / 4;
    uint32_t start = mp_obj_get_int(start_in);
    if (frames == 0 || start >= frames) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad audio ring"));
    }
    ctx->ring = ring.buf;
    ctx->ring_frames = frames;
    ctx->ring_pos = start;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_set_audio_obj, mod_set_audio);

static mp_obj_t mod_set_joypad(mp_obj_t st_in, mp_obj_t mask_in) {
    get_ctx(st_in)->nes.pad = (uint8_t)mp_obj_get_int(mask_in);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_set_joypad_obj, mod_set_joypad);

static mp_obj_t mod_run_frames(mp_obj_t st_in, mp_obj_t n_in) {
    return mp_obj_new_int(ctx_run_frames(get_ctx(st_in), mp_obj_get_int(n_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_run_frames_obj, mod_run_frames);

// First call turns CRC tracking on (it costs time) and returns 0; later calls
// return the CRC-32 of the last complete frame's 256x240 NES color indices.
static mp_obj_t mod_frame_crc(mp_obj_t st_in) {
    ctx_t *ctx = get_ctx(st_in);
    ctx->crc_on = 1;
    return mp_obj_new_int_from_uint(ctx->last_crc);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_frame_crc_obj, mod_frame_crc);

// error(st) -> (code, addr); code 1 means the CPU hit a jam opcode at addr.
static mp_obj_t mod_error(mp_obj_t st_in) {
    nes_t *n = &get_ctx(st_in)->nes;
    mp_obj_t t[2] = {mp_obj_new_int(n->err), mp_obj_new_int(n->err_addr)};
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_error_obj, mod_error);

mp_obj_t mpy_init(mp_obj_fun_bc_t *self, size_t n_args, size_t n_kw, mp_obj_t *args) {
    MP_DYNRUNTIME_INIT_ENTRY
    mp_store_global(MP_QSTR_STATE_SIZE, MP_OBJ_NEW_SMALL_INT(sizeof(ctx_t)));
    mp_store_global(MP_QSTR_WIDTH, MP_OBJ_NEW_SMALL_INT(NES_W));
    mp_store_global(MP_QSTR_HEIGHT, MP_OBJ_NEW_SMALL_INT(NES_H));
    mp_store_global(MP_QSTR_AUDIO_RATE, MP_OBJ_NEW_SMALL_INT(NES_AUDIO_RATE));
    mp_store_global(MP_QSTR_AUDIO_SAMPLES_PER_FRAME, MP_OBJ_NEW_SMALL_INT(NES_AUDIO_SAMPLES));
    mp_store_global(MP_QSTR_JOYPAD_A, MP_OBJ_NEW_SMALL_INT(NES_A));
    mp_store_global(MP_QSTR_JOYPAD_B, MP_OBJ_NEW_SMALL_INT(NES_B));
    mp_store_global(MP_QSTR_JOYPAD_SELECT, MP_OBJ_NEW_SMALL_INT(NES_SELECT));
    mp_store_global(MP_QSTR_JOYPAD_START, MP_OBJ_NEW_SMALL_INT(NES_START));
    mp_store_global(MP_QSTR_JOYPAD_UP, MP_OBJ_NEW_SMALL_INT(NES_UP));
    mp_store_global(MP_QSTR_JOYPAD_DOWN, MP_OBJ_NEW_SMALL_INT(NES_DOWN));
    mp_store_global(MP_QSTR_JOYPAD_LEFT, MP_OBJ_NEW_SMALL_INT(NES_LEFT));
    mp_store_global(MP_QSTR_JOYPAD_RIGHT, MP_OBJ_NEW_SMALL_INT(NES_RIGHT));
    mp_store_global(MP_QSTR_init, MP_OBJ_FROM_PTR(&mod_init_obj));
    mp_store_global(MP_QSTR_info, MP_OBJ_FROM_PTR(&mod_info_obj));
    mp_store_global(MP_QSTR_save_size, MP_OBJ_FROM_PTR(&mod_save_size_obj));
    mp_store_global(MP_QSTR_set_cart_ram, MP_OBJ_FROM_PTR(&mod_set_cart_ram_obj));
    mp_store_global(MP_QSTR_set_output, MP_OBJ_FROM_PTR(&mod_set_output_obj));
    mp_store_global(MP_QSTR_set_joypad, MP_OBJ_FROM_PTR(&mod_set_joypad_obj));
    mp_store_global(MP_QSTR_set_audio, MP_OBJ_FROM_PTR(&mod_set_audio_obj));
    mp_store_global(MP_QSTR_run_frames, MP_OBJ_FROM_PTR(&mod_run_frames_obj));
    mp_store_global(MP_QSTR_frame_crc, MP_OBJ_FROM_PTR(&mod_frame_crc_obj));
    mp_store_global(MP_QSTR_error, MP_OBJ_FROM_PTR(&mod_error_obj));
    MP_DYNRUNTIME_INIT_EXIT
}

#endif
