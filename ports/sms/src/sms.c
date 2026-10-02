// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Native module `sms`: a Sega Master System / Game Gear / SG-1000 emulator
// (smscore.h) as a CircuitPython native .mpy, following the nes module.
//
// Every buffer lives in a Python-owned bytearray and is passed in, so the GC
// sees all of it and nothing has to be allocated from native code:
//
//   st = bytearray(sms.STATE_SIZE)               # core + context, keep alive
//   sms.init(st, rom, sms.SYSTEM_SMS)            # or SYSTEM_GG, SYSTEM_SG
//   sms.set_cart_ram(st, bytearray(sms.save_size(st)))  # cart RAM, keep alive
//   sms.set_output(st, fb, width, height, bpp)   # optional
//   sms.set_joypad(st, mask)                     # 1 = pressed, see JOYPAD_*
//   sms.run_frames(st, n)                        # -> frames run
//   sms.cart_ram_used(st)                        # True once the game used it
//   sms.frame_crc(st)                            # CRC-32 of the last frame
//   sms.set_audio(st, ring, start)               # optional, see below
//
// Output formats: bpp 8 is RGB332 (picodvi 8-bit), bpp 16 is RGB565.
// Layout is picked from the framebuffer size: SMS and SG-1000 are drawn 1:1
// and centered (256x192, or 256x224/240 for the SMS2 tall modes). Game Gear's
// 160x144 is doubled horizontally when it fits, and scaled vertically by 2,
// 1.5 or 1, whichever fits, then centered. On 320x240 that is 320x216.
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

#include "smscore.h"

typedef struct {
    sms_t sms;  // first, so sms_t * and ctx_t * are the same pointer
    uint8_t *fb;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t bpp;
    uint32_t x0;
    uint32_t y0;
    uint32_t hscale;    // 1 or 2
    uint32_t vscale2;   // vertical scale x2: 2 (1x), 3 (1.5x), 4 (2x)
    uint32_t cur_height;
    int16_t *ring;
    uint32_t ring_frames;
    uint32_t ring_pos;
    uint32_t crc_on;
    uint32_t crc;
    uint32_t last_crc;
    uint32_t lut_ver;   // rgb_ver the LUTs were built from, +1
    uint16_t lut16[128];
    uint8_t lut8[128];
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

// Picks where the picture goes in the framebuffer, and clears it when that
// changes (SMS tall modes).
static void ctx_layout(ctx_t *ctx) {
    sms_t *n = &ctx->sms;
    ctx->cur_height = n->height;
    if (ctx->fb == NULL) {
        return;
    }
    uint32_t w = ctx->fb_width, h = ctx->fb_height;
    if (n->system == SMS_SYS_GG) {
        ctx->hscale = w >= 320 ? 2 : 1;
        ctx->vscale2 = h >= 288 ? 4 : (h >= 216 ? 3 : 2);
        ctx->x0 = (w - 160 * ctx->hscale) / 2;
        ctx->y0 = (h - 144 * ctx->vscale2 / 2) / 2;
    } else {
        ctx->hscale = 1;
        ctx->vscale2 = 2;
        ctx->x0 = (w - SMS_W) / 2;
        ctx->y0 = h > n->height ? (h - n->height) / 2 : 0;
    }
    memset(ctx->fb, 0, w * h * (ctx->bpp / 8));
}

static void sms_draw_line(sms_t *n, int line, const uint8_t *px) {
    ctx_t *ctx = (ctx_t *)n;
    if (ctx->lut_ver != n->rgb_ver + 1) {
        for (int i = 0; i < 128; i++) {
            uint32_t c = n->rgb[i & 63];
            ctx->lut16[i] = c;
            ctx->lut8[i] = ((c >> 8) & 0xE0) | ((c >> 6) & 0x1C) | ((c >> 3) & 0x03);
        }
        ctx->lut_ver = n->rgb_ver + 1;
    }
    int gg = n->system == SMS_SYS_GG;
    const uint8_t *src = gg ? px + 48 : px;
    uint32_t w = gg ? 160 : SMS_W;
    int sl = gg ? line - 24 : line;  // line within the visible picture
    if (ctx->crc_on) {
        uint16_t rgb[SMS_W];
        for (uint32_t x = 0; x < w; x++) {
            rgb[x] = ctx->lut16[src[x]];
        }
        if (sl == 0) {
            ctx->crc = 0xFFFFFFFF;
        }
        ctx->crc = crc32_update(ctx->crc, (const uint8_t *)rgb, w * 2);
        if (sl == (gg ? 143 : (int)n->height - 1)) {
            ctx->last_crc = ~ctx->crc;
        }
    }
    if (ctx->fb == NULL) {
        return;
    }
    if (sl == 0 && n->height != ctx->cur_height) {
        ctx_layout(ctx);
    }
    // Output rows for this source line.
    uint32_t y, rows;
    if (ctx->vscale2 == 4) {
        y = sl * 2;
        rows = 2;
    } else if (ctx->vscale2 == 3) {
        y = (sl & 1) ? (3 * sl + 1) / 2 : 3 * sl / 2;
        rows = (sl & 1) ? 1 : 2;
    } else {
        y = sl;
        rows = 1;
    }
    y += ctx->y0;
    if (y >= ctx->fb_height) {
        return;
    }
    if (y + rows > ctx->fb_height) {
        rows = 1;
    }
    uint32_t bytes = ctx->bpp / 8;
    uint8_t *row = ctx->fb + (y * ctx->fb_width + ctx->x0) * bytes;
    if (ctx->bpp == 8) {
        const uint8_t *lut = ctx->lut8;
        uint8_t *d = row;
        if (ctx->hscale == 2) {
            for (uint32_t x = 0; x < w; x++) {
                uint8_t c = lut[src[x]];
                d[2 * x] = c;
                d[2 * x + 1] = c;
            }
        } else {
            for (uint32_t x = 0; x < w; x++) {
                d[x] = lut[src[x]];
            }
        }
    } else {
        const uint16_t *lut = ctx->lut16;
        uint16_t *d = (uint16_t *)row;
        if (ctx->hscale == 2) {
            for (uint32_t x = 0; x < w; x++) {
                uint16_t c = lut[src[x]];
                d[2 * x] = c;
                d[2 * x + 1] = c;
            }
        } else {
            for (uint32_t x = 0; x < w; x++) {
                d[x] = lut[src[x]];
            }
        }
    }
    if (rows == 2) {
        memcpy(row + ctx->fb_width * bytes, row, w * ctx->hscale * bytes);
    }
}

// Core entry points, shared by the host harness and the native module.
static int ctx_init(ctx_t *ctx, const uint8_t *rom, uint32_t len, int system) {
    memset(ctx, 0, sizeof(*ctx));
    return sms_init(&ctx->sms, rom, len, system);
}

static uint32_t ctx_run_frames(ctx_t *ctx, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        sms_run_frame(&ctx->sms);
        if (ctx->ring != NULL) {
            // Mono samples duplicated to stereo, copied into the ring with wrap.
            const int16_t *src = ctx->sms.audio;
            int16_t *ring = ctx->ring;
            uint32_t pos = ctx->ring_pos;
            for (uint32_t k = 0; k < SMS_AUDIO_SAMPLES; k++) {
                ring[pos * 2] = src[k];
                ring[pos * 2 + 1] = src[k];
                if (++pos == ctx->ring_frames) {
                    pos = 0;
                }
            }
            ctx->ring_pos = pos;
        }
    }
    return count;
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

static mp_obj_t mod_init(mp_obj_t st_in, mp_obj_t rom_in, mp_obj_t system_in) {
    ctx_t *ctx = get_ctx(st_in);
    mp_buffer_info_t rom;
    mp_get_buffer_raise(rom_in, &rom, MP_BUFFER_READ);
    uint32_t system = mp_obj_get_int(system_in);
    if (system > SMS_SYS_SG) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad system"));
    }
    return mp_obj_new_int(ctx_init(ctx, rom.buf, rom.len, system));
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_init_obj, mod_init);

// info(st) -> (system, mapper, rom_bytes); mapper 0 none, 1 Sega, 2 Codemasters
static mp_obj_t mod_info(mp_obj_t st_in) {
    sms_t *n = &get_ctx(st_in)->sms;
    mp_obj_t t[3] = {
        mp_obj_new_int(n->system), mp_obj_new_int(n->mapper), mp_obj_new_int(n->rom_size),
    };
    return mp_obj_new_tuple(3, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_info_obj, mod_info);

// Cart RAM size a save buffer needs: 32768 for mapped carts, else 0.
static mp_obj_t mod_save_size(mp_obj_t st_in) {
    return mp_obj_new_int(get_ctx(st_in)->sms.mapper == SMS_MAP_NONE ? 0 : 0x8000);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_save_size_obj, mod_save_size);

// Use the caller's buffer (>= 32768 bytes) as cart RAM, so it can be saved.
// Its current contents are kept, so load the .sav into it first.
static mp_obj_t mod_set_cart_ram(mp_obj_t st_in, mp_obj_t ram_in) {
    ctx_t *ctx = get_ctx(st_in);
    mp_buffer_info_t ram;
    mp_get_buffer_raise(ram_in, &ram, MP_BUFFER_RW);
    if (ram.len < 0x8000) {
        mp_raise_ValueError(MP_ERROR_TEXT("cart RAM must be 32768 bytes"));
    }
    ctx->sms.cart_ram = ram.buf;
    map_update(&ctx->sms);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_set_cart_ram_obj, mod_set_cart_ram);

static mp_obj_t mod_cart_ram_used(mp_obj_t st_in) {
    return mp_obj_new_bool(get_ctx(st_in)->sms.sram_used);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_cart_ram_used_obj, mod_cart_ram_used);

// set_output(st, fb, width, height, bpp); fb=None disables drawing.
static mp_obj_t mod_set_output(size_t n_args, const mp_obj_t *args) {
    ctx_t *ctx = get_ctx(args[0]);
    if (args[1] == mp_const_none) {
        ctx->fb = NULL;
        return mp_const_none;
    }
    mp_buffer_info_t fb;
    mp_get_buffer_raise(args[1], &fb, MP_BUFFER_WRITE);
    uint32_t width = mp_obj_get_int(args[2]);
    uint32_t height = mp_obj_get_int(args[3]);
    uint32_t bpp = mp_obj_get_int(args[4]);
    if ((bpp != 8 && bpp != 16) || width < SMS_W || height < 144 ||
        width * height * (bpp / 8) > fb.len) {
        mp_raise_ValueError(MP_ERROR_TEXT("output does not fit"));
    }
    ctx->fb = fb.buf;
    ctx->fb_width = width;
    ctx->fb_height = height;
    ctx->bpp = bpp;
    ctx_layout(ctx);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_set_output_obj, 5, 5, mod_set_output);

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
    sms_set_pad(&get_ctx(st_in)->sms, (uint8_t)mp_obj_get_int(mask_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_set_joypad_obj, mod_set_joypad);

static mp_obj_t mod_run_frames(mp_obj_t st_in, mp_obj_t n_in) {
    return mp_obj_new_int(ctx_run_frames(get_ctx(st_in), mp_obj_get_int(n_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_run_frames_obj, mod_run_frames);

// First call turns CRC tracking on (it costs time) and returns 0; later calls
// return the CRC-32 of the last complete frame's visible RGB565 pixels.
static mp_obj_t mod_frame_crc(mp_obj_t st_in) {
    ctx_t *ctx = get_ctx(st_in);
    ctx->crc_on = 1;
    return mp_obj_new_int_from_uint(ctx->last_crc);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_frame_crc_obj, mod_frame_crc);

mp_obj_t mpy_init(mp_obj_fun_bc_t *self, size_t n_args, size_t n_kw, mp_obj_t *args) {
    MP_DYNRUNTIME_INIT_ENTRY
    mp_store_global(MP_QSTR_STATE_SIZE, MP_OBJ_NEW_SMALL_INT(sizeof(ctx_t)));
    mp_store_global(MP_QSTR_SYSTEM_SMS, MP_OBJ_NEW_SMALL_INT(SMS_SYS_SMS));
    mp_store_global(MP_QSTR_SYSTEM_GG, MP_OBJ_NEW_SMALL_INT(SMS_SYS_GG));
    mp_store_global(MP_QSTR_SYSTEM_SG, MP_OBJ_NEW_SMALL_INT(SMS_SYS_SG));
    mp_store_global(MP_QSTR_AUDIO_RATE, MP_OBJ_NEW_SMALL_INT(SMS_AUDIO_RATE));
    mp_store_global(MP_QSTR_AUDIO_SAMPLES_PER_FRAME, MP_OBJ_NEW_SMALL_INT(SMS_AUDIO_SAMPLES));
    mp_store_global(MP_QSTR_JOYPAD_UP, MP_OBJ_NEW_SMALL_INT(SMS_UP));
    mp_store_global(MP_QSTR_JOYPAD_DOWN, MP_OBJ_NEW_SMALL_INT(SMS_DOWN));
    mp_store_global(MP_QSTR_JOYPAD_LEFT, MP_OBJ_NEW_SMALL_INT(SMS_LEFT));
    mp_store_global(MP_QSTR_JOYPAD_RIGHT, MP_OBJ_NEW_SMALL_INT(SMS_RIGHT));
    mp_store_global(MP_QSTR_JOYPAD_1, MP_OBJ_NEW_SMALL_INT(SMS_B1));
    mp_store_global(MP_QSTR_JOYPAD_2, MP_OBJ_NEW_SMALL_INT(SMS_B2));
    mp_store_global(MP_QSTR_JOYPAD_START, MP_OBJ_NEW_SMALL_INT(SMS_START));
    mp_store_global(MP_QSTR_init, MP_OBJ_FROM_PTR(&mod_init_obj));
    mp_store_global(MP_QSTR_info, MP_OBJ_FROM_PTR(&mod_info_obj));
    mp_store_global(MP_QSTR_save_size, MP_OBJ_FROM_PTR(&mod_save_size_obj));
    mp_store_global(MP_QSTR_set_cart_ram, MP_OBJ_FROM_PTR(&mod_set_cart_ram_obj));
    mp_store_global(MP_QSTR_cart_ram_used, MP_OBJ_FROM_PTR(&mod_cart_ram_used_obj));
    mp_store_global(MP_QSTR_set_output, MP_OBJ_FROM_PTR(&mod_set_output_obj));
    mp_store_global(MP_QSTR_set_joypad, MP_OBJ_FROM_PTR(&mod_set_joypad_obj));
    mp_store_global(MP_QSTR_set_audio, MP_OBJ_FROM_PTR(&mod_set_audio_obj));
    mp_store_global(MP_QSTR_run_frames, MP_OBJ_FROM_PTR(&mod_run_frames_obj));
    mp_store_global(MP_QSTR_frame_crc, MP_OBJ_FROM_PTR(&mod_frame_crc_obj));
    MP_DYNRUNTIME_INIT_EXIT
}

#endif
