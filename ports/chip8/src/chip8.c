// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Native module `chip8`: a CHIP-8 / SUPER-CHIP interpreter written for this
// port, as a CircuitPython native .mpy.
//
// Every buffer lives in a Python-owned bytearray and is passed in, so the GC
// sees all of it and nothing has to be allocated from native code:
//
//   st = bytearray(chip8.STATE_SIZE)          # whole machine, keep alive
//   chip8.init(st, rom, seed)                 # -> 0, or 1 if the ROM is too big
//   chip8.config(st, quirks, ipf)             # QUIRK_* bits, instructions per frame
//   chip8.set_output(st, fb, width, height, bpp)   # optional
//   chip8.set_colors(st, fg, bg)              # pixel values in the fb format
//   chip8.set_keys(st, mask)                  # bit k set = hex key k held
//   chip8.run_frames(st, n)                   # -> frames run, stops on error/exit
//   chip8.frame_crc(st)                       # CRC-32 of the 128x64 display
//   chip8.set_audio(st, ring, start)          # optional, see below
//   chip8.set_beep(st, hz, amplitude)         # square wave pitch and level
//
// Output formats: bpp 8 is RGB332 (picodvi 8-bit), bpp 16 is RGB565. The
// 64x32 (lores) or 128x64 (hires) screen is drawn at the largest integer scale
// that fits, centered, only on frames where it changed.
//
// Sound: a square wave while the sound timer is non-zero. Every frame appends
// AUDIO_SAMPLES_PER_FRAME stereo int16 frames to the ring, wrapping. Make the
// ring the buffer of a looping, double buffered audiocore.RawSample and start
// writing one half ahead of playback.

#ifdef HOST_TEST
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#else
#include "py/dynruntime.h"

// dynruntime.h does not map these; GCC emits calls for loops and struct copies.
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

#define MEM_SIZE 4096
#define PROG_START 0x200
#define FONT_SMALL 0x050    // 16 glyphs, 5 bytes each
#define FONT_BIG 0x0A0      // SUPER-CHIP 10-byte digits 0-9
#define DISP_W 128          // display is always kept at hires resolution;
#define DISP_H 64           // a lores pixel is a 2x2 block
#define AUDIO_RATE 22050
#define AUDIO_SAMPLES 367   // per frame: 60.08 Hz pacing

// Quirk bits. See the Timendus quirks test for what each one means.
#define QUIRK_VF_RESET 0x01      // 8XY1/2/3 clear VF
#define QUIRK_MEMORY 0x02        // FX55/FX65 advance I
#define QUIRK_DISPLAY_WAIT 0x04  // DXYN ends the frame (max 60 sprites/s)
#define QUIRK_CLIPPING 0x08      // sprites clip at the edges instead of wrapping
#define QUIRK_SHIFTING 0x10      // 8XY6/8XYE shift VX, ignore VY
#define QUIRK_JUMPING 0x20       // BXNN jumps to XNN + VX
#define QUIRKS_CHIP8 (QUIRK_VF_RESET | QUIRK_MEMORY | QUIRK_DISPLAY_WAIT | QUIRK_CLIPPING)
#define QUIRKS_SCHIP (QUIRK_CLIPPING | QUIRK_SHIFTING | QUIRK_JUMPING)

// err codes from run_frames / error().
#define ERR_NONE 0
#define ERR_OPCODE 1    // unknown instruction at err_addr
#define ERR_STACK 2     // call stack over or underflow
#define ERR_EXIT 3      // SUPER-CHIP 00FD exit

typedef struct {
    uint8_t mem[MEM_SIZE];
    uint8_t disp[DISP_H * DISP_W];  // 0 or 1 per hires pixel
    uint8_t v[16];
    uint8_t rpl[16];                // SUPER-CHIP FX75/FX85 flags
    uint16_t stack[16];
    uint32_t pc;
    uint32_t i;
    uint32_t sp;
    uint32_t dt;
    uint32_t st;
    uint32_t hires;
    uint32_t keys;
    int32_t wait_key;               // FX0A: key seen down, waiting for release
    uint32_t quirks;
    uint32_t ipf;
    uint32_t rng;
    uint32_t err;
    uint32_t err_addr;
    uint32_t dirty;
    // output
    uint8_t *fb;
    uint32_t fb_w;
    uint32_t fb_h;
    uint32_t bpp;
    uint32_t fg;
    uint32_t bg;
    uint32_t shown;                 // layout last drawn: 0 none, 1 lores, 2 hires
    // audio
    int16_t *ring;
    uint32_t ring_frames;
    uint32_t ring_pos;
    uint32_t half_period;           // samples per half cycle of the square wave
    uint32_t phase;
    int32_t amp;
} ctx_t;

static const uint8_t font_small[80] = {
    0xF0, 0x90, 0x90, 0x90, 0xF0, 0x20, 0x60, 0x20, 0x20, 0x70,
    0xF0, 0x10, 0xF0, 0x80, 0xF0, 0xF0, 0x10, 0xF0, 0x10, 0xF0,
    0x90, 0x90, 0xF0, 0x10, 0x10, 0xF0, 0x80, 0xF0, 0x10, 0xF0,
    0xF0, 0x80, 0xF0, 0x90, 0xF0, 0xF0, 0x10, 0x20, 0x40, 0x40,
    0xF0, 0x90, 0xF0, 0x90, 0xF0, 0xF0, 0x90, 0xF0, 0x10, 0xF0,
    0xF0, 0x90, 0xF0, 0x90, 0x90, 0xE0, 0x90, 0xE0, 0x90, 0xE0,
    0xF0, 0x80, 0x80, 0x80, 0xF0, 0xE0, 0x90, 0x90, 0x90, 0xE0,
    0xF0, 0x80, 0xF0, 0x80, 0xF0, 0xF0, 0x80, 0xF0, 0x80, 0x80,
};

static const uint8_t font_big[100] = {
    0x3C, 0x7E, 0xE7, 0xC3, 0xC3, 0xC3, 0xC3, 0xE7, 0x7E, 0x3C,
    0x18, 0x38, 0x58, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C,
    0x3E, 0x7F, 0xC3, 0x06, 0x0C, 0x18, 0x30, 0x60, 0xFF, 0xFF,
    0x3C, 0x7E, 0xC3, 0x03, 0x0E, 0x0E, 0x03, 0xC3, 0x7E, 0x3C,
    0x06, 0x0E, 0x1E, 0x36, 0x66, 0xC6, 0xFF, 0xFF, 0x06, 0x06,
    0xFF, 0xFF, 0xC0, 0xC0, 0xFC, 0xFE, 0x03, 0xC3, 0x7E, 0x3C,
    0x3E, 0x7C, 0xC0, 0xC0, 0xFC, 0xFE, 0xC3, 0xC3, 0x7E, 0x3C,
    0xFF, 0xFF, 0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x60, 0x60,
    0x3C, 0x7E, 0xC3, 0xC3, 0x7E, 0x7E, 0xC3, 0xC3, 0x7E, 0x3C,
    0x3C, 0x7E, 0xC3, 0xC3, 0x7F, 0x3F, 0x03, 0x03, 0x3E, 0x7C,
};

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFF;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320 & -(c & 1));
        }
    }
    return ~c;
}

static uint32_t next_rand(ctx_t *ctx) {
    // xorshift32
    uint32_t x = ctx->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    ctx->rng = x;
    return x;
}

// ---- display ------------------------------------------------------------------

// Draw an 8 or 16 pixel wide sprite row set. Returns 1 on any collision.
static uint32_t draw_sprite(ctx_t *ctx, uint32_t vx, uint32_t vy, uint32_t n) {
    const uint32_t s = ctx->hires ? 1 : 2;       // hires pixels per logical pixel
    const uint32_t w = DISP_W / s;
    const uint32_t h = DISP_H / s;
    const uint32_t clip = ctx->quirks & QUIRK_CLIPPING;
    const uint32_t wide = n == 0;                 // DXY0: 16x16
    const uint32_t rows = wide ? 16 : n;
    const uint32_t cols = wide ? 16 : 8;
    const uint32_t x0 = vx % w;
    const uint32_t y0 = vy % h;
    uint32_t addr = ctx->i;
    uint32_t hit = 0;
    for (uint32_t r = 0; r < rows; r++) {
        uint32_t bits = ctx->mem[addr++ & (MEM_SIZE - 1)];
        if (wide) {
            bits = (bits << 8) | ctx->mem[addr++ & (MEM_SIZE - 1)];
        }
        uint32_t y = y0 + r;
        if (y >= h) {
            if (clip) {
                break;
            }
            y -= h;
        }
        for (uint32_t c = 0; c < cols; c++) {
            if (!(bits & (1u << (cols - 1 - c)))) {
                continue;
            }
            uint32_t x = x0 + c;
            if (x >= w) {
                if (clip) {
                    break;
                }
                x -= w;
            }
            uint8_t *p = &ctx->disp[y * s * DISP_W + x * s];
            hit |= p[0];
            p[0] ^= 1;
            if (s == 2) {
                p[1] ^= 1;
                p[DISP_W] ^= 1;
                p[DISP_W + 1] ^= 1;
            }
        }
    }
    ctx->dirty = 1;
    return hit;
}

static void scroll_down(ctx_t *ctx, uint32_t n) {
    if (n > DISP_H) {
        n = DISP_H;
    }
    memmove(ctx->disp + n * DISP_W, ctx->disp, (DISP_H - n) * DISP_W);
    memset(ctx->disp, 0, n * DISP_W);
    ctx->dirty = 1;
}

// dir > 0 scrolls right, < 0 left, by n hires pixels.
static void scroll_side(ctx_t *ctx, int dir, uint32_t n) {
    for (uint32_t y = 0; y < DISP_H; y++) {
        uint8_t *row = ctx->disp + y * DISP_W;
        if (dir > 0) {
            memmove(row + n, row, DISP_W - n);
            memset(row, 0, n);
        } else {
            memmove(row, row + n, DISP_W - n);
            memset(row + DISP_W - n, 0, n);
        }
    }
    ctx->dirty = 1;
}

static void fill_fb(ctx_t *ctx, uint32_t color) {
    uint32_t n = ctx->fb_w * ctx->fb_h;
    if (ctx->bpp == 8) {
        memset(ctx->fb, (int)color, n);
    } else {
        uint16_t *p = (uint16_t *)ctx->fb;
        for (uint32_t k = 0; k < n; k++) {
            p[k] = (uint16_t)color;
        }
    }
}

// Blit the display at the largest integer scale that fits, centered.
static void render(ctx_t *ctx) {
    const uint32_t step = ctx->hires ? 1 : 2;     // sample every hires pixel or every other
    const uint32_t lw = DISP_W / step;
    const uint32_t lh = DISP_H / step;
    uint32_t s = ctx->fb_w / lw;
    if (ctx->fb_h / lh < s) {
        s = ctx->fb_h / lh;
    }
    if (s == 0) {
        return;
    }
    const uint32_t layout = ctx->hires ? 2 : 1;
    if (ctx->shown != layout) {
        fill_fb(ctx, ctx->bg);
        ctx->shown = layout;
    }
    const uint32_t x0 = (ctx->fb_w - lw * s) / 2;
    const uint32_t y0 = (ctx->fb_h - lh * s) / 2;
    const uint32_t w = ctx->fb_w;
    for (uint32_t ly = 0; ly < lh; ly++) {
        const uint8_t *src = ctx->disp + ly * step * DISP_W;
        const uint32_t y = y0 + ly * s;
        if (ctx->bpp == 8) {
            const uint8_t fg = (uint8_t)ctx->fg, bg = (uint8_t)ctx->bg;
            uint8_t *first = ctx->fb + y * w + x0;
            uint8_t *dst = first;
            for (uint32_t lx = 0; lx < lw; lx++) {
                uint8_t c = src[lx * step] ? fg : bg;
                for (uint32_t k = 0; k < s; k++) {
                    *dst++ = c;
                }
            }
            for (uint32_t k = 1; k < s; k++) {
                memcpy(first + k * w, first, lw * s);
            }
        } else {
            const uint16_t fg = (uint16_t)ctx->fg, bg = (uint16_t)ctx->bg;
            uint16_t *first = (uint16_t *)ctx->fb + y * w + x0;
            uint16_t *dst = first;
            for (uint32_t lx = 0; lx < lw; lx++) {
                uint16_t c = src[lx * step] ? fg : bg;
                for (uint32_t k = 0; k < s; k++) {
                    *dst++ = c;
                }
            }
            for (uint32_t k = 1; k < s; k++) {
                memcpy(first + k * w, first, lw * s * 2);
            }
        }
    }
}

// ---- CPU ----------------------------------------------------------------------

// Run one instruction. Returns 0 to keep going, 1 to end this frame early
// (display wait, waiting for a key, error).
static int step(ctx_t *ctx) {
    uint8_t *m = ctx->mem;
    uint8_t *v = ctx->v;
    const uint32_t pc = ctx->pc;
    const uint32_t op = (m[pc] << 8) | m[(pc + 1) & (MEM_SIZE - 1)];
    const uint32_t x = (op >> 8) & 0xF;
    const uint32_t y = (op >> 4) & 0xF;
    const uint32_t n = op & 0xF;
    const uint32_t nn = op & 0xFF;
    const uint32_t nnn = op & 0xFFF;
    ctx->pc = (pc + 2) & (MEM_SIZE - 1);

    switch (op >> 12) {
        case 0x0:
            if (op == 0x00E0) {
                memset(ctx->disp, 0, sizeof(ctx->disp));
                ctx->dirty = 1;
            } else if (op == 0x00EE) {
                if (ctx->sp == 0) {
                    goto stack_err;
                }
                ctx->pc = ctx->stack[--ctx->sp];
            } else if ((op & 0xFFF0) == 0x00C0) {
                scroll_down(ctx, ctx->hires ? n : n * 2);
            } else if (op == 0x00FB) {
                scroll_side(ctx, 1, ctx->hires ? 4 : 8);
            } else if (op == 0x00FC) {
                scroll_side(ctx, -1, ctx->hires ? 4 : 8);
            } else if (op == 0x00FD) {
                ctx->err = ERR_EXIT;
                ctx->err_addr = pc;
                return 1;
            } else if (op == 0x00FE) {
                ctx->hires = 0;
                ctx->dirty = 1;
            } else if (op == 0x00FF) {
                ctx->hires = 1;
                ctx->dirty = 1;
            } else {
                goto bad_op;  // 0NNN machine code calls are not supported
            }
            break;
        case 0x1:
            ctx->pc = nnn;
            break;
        case 0x2:
            if (ctx->sp >= 16) {
                goto stack_err;
            }
            ctx->stack[ctx->sp++] = (uint16_t)ctx->pc;
            ctx->pc = nnn;
            break;
        case 0x3:
            if (v[x] == nn) {
                ctx->pc = (ctx->pc + 2) & (MEM_SIZE - 1);
            }
            break;
        case 0x4:
            if (v[x] != nn) {
                ctx->pc = (ctx->pc + 2) & (MEM_SIZE - 1);
            }
            break;
        case 0x5:
            if (v[x] == v[y]) {
                ctx->pc = (ctx->pc + 2) & (MEM_SIZE - 1);
            }
            break;
        case 0x6:
            v[x] = (uint8_t)nn;
            break;
        case 0x7:
            v[x] = (uint8_t)(v[x] + nn);
            break;
        case 0x8: {
            // VF is written last so VF as an operand behaves like the COSMAC.
            uint32_t a = v[x], b = v[y], r, f;
            switch (n) {
                case 0x0:
                    v[x] = (uint8_t)b;
                    break;
                case 0x1:
                    v[x] = (uint8_t)(a | b);
                    if (ctx->quirks & QUIRK_VF_RESET) {
                        v[0xF] = 0;
                    }
                    break;
                case 0x2:
                    v[x] = (uint8_t)(a & b);
                    if (ctx->quirks & QUIRK_VF_RESET) {
                        v[0xF] = 0;
                    }
                    break;
                case 0x3:
                    v[x] = (uint8_t)(a ^ b);
                    if (ctx->quirks & QUIRK_VF_RESET) {
                        v[0xF] = 0;
                    }
                    break;
                case 0x4:
                    r = a + b;
                    v[x] = (uint8_t)r;
                    v[0xF] = r > 0xFF;
                    break;
                case 0x5:
                    f = a >= b;
                    v[x] = (uint8_t)(a - b);
                    v[0xF] = (uint8_t)f;
                    break;
                case 0x6:
                    r = (ctx->quirks & QUIRK_SHIFTING) ? a : b;
                    v[x] = (uint8_t)(r >> 1);
                    v[0xF] = r & 1;
                    break;
                case 0x7:
                    f = b >= a;
                    v[x] = (uint8_t)(b - a);
                    v[0xF] = (uint8_t)f;
                    break;
                case 0xE:
                    r = (ctx->quirks & QUIRK_SHIFTING) ? a : b;
                    v[x] = (uint8_t)(r << 1);
                    v[0xF] = (r >> 7) & 1;
                    break;
                default:
                    goto bad_op;
            }
            break;
        }
        case 0x9:
            if (v[x] != v[y]) {
                ctx->pc = (ctx->pc + 2) & (MEM_SIZE - 1);
            }
            break;
        case 0xA:
            ctx->i = nnn;
            break;
        case 0xB:
            ctx->pc = (nnn + v[(ctx->quirks & QUIRK_JUMPING) ? x : 0]) & (MEM_SIZE - 1);
            break;
        case 0xC:
            v[x] = (uint8_t)(next_rand(ctx) >> 24) & nn;
            break;
        case 0xD:
            v[0xF] = (uint8_t)draw_sprite(ctx, v[x], v[y], n);
            if (ctx->quirks & QUIRK_DISPLAY_WAIT) {
                return 1;
            }
            break;
        case 0xE:
            if (nn == 0x9E) {
                if ((ctx->keys >> (v[x] & 0xF)) & 1) {
                    ctx->pc = (ctx->pc + 2) & (MEM_SIZE - 1);
                }
            } else if (nn == 0xA1) {
                if (!((ctx->keys >> (v[x] & 0xF)) & 1)) {
                    ctx->pc = (ctx->pc + 2) & (MEM_SIZE - 1);
                }
            } else {
                goto bad_op;
            }
            break;
        case 0xF:
            switch (nn) {
                case 0x07:
                    v[x] = (uint8_t)ctx->dt;
                    break;
                case 0x0A:
                    // Wait for a key to go down and come back up, like the COSMAC VIP.
                    if (ctx->wait_key < 0) {
                        for (int k = 0; k < 16; k++) {
                            if ((ctx->keys >> k) & 1) {
                                ctx->wait_key = k;
                                break;
                            }
                        }
                    } else if (!((ctx->keys >> ctx->wait_key) & 1)) {
                        v[x] = (uint8_t)ctx->wait_key;
                        ctx->wait_key = -1;
                        break;
                    }
                    ctx->pc = pc;
                    return 1;
                case 0x15:
                    ctx->dt = v[x];
                    break;
                case 0x18:
                    ctx->st = v[x];
                    break;
                case 0x1E:
                    ctx->i = (ctx->i + v[x]) & 0xFFFF;
                    break;
                case 0x29:
                    ctx->i = FONT_SMALL + (v[x] & 0xF) * 5;
                    break;
                case 0x30:
                    ctx->i = FONT_BIG + (v[x] % 10) * 10;
                    break;
                case 0x33:
                    m[ctx->i & (MEM_SIZE - 1)] = v[x] / 100;
                    m[(ctx->i + 1) & (MEM_SIZE - 1)] = (v[x] / 10) % 10;
                    m[(ctx->i + 2) & (MEM_SIZE - 1)] = v[x] % 10;
                    break;
                case 0x55:
                    for (uint32_t k = 0; k <= x; k++) {
                        m[(ctx->i + k) & (MEM_SIZE - 1)] = v[k];
                    }
                    if (ctx->quirks & QUIRK_MEMORY) {
                        ctx->i = (ctx->i + x + 1) & 0xFFFF;
                    }
                    break;
                case 0x65:
                    for (uint32_t k = 0; k <= x; k++) {
                        v[k] = m[(ctx->i + k) & (MEM_SIZE - 1)];
                    }
                    if (ctx->quirks & QUIRK_MEMORY) {
                        ctx->i = (ctx->i + x + 1) & 0xFFFF;
                    }
                    break;
                case 0x75:
                    for (uint32_t k = 0; k <= x; k++) {
                        ctx->rpl[k] = v[k];
                    }
                    break;
                case 0x85:
                    for (uint32_t k = 0; k <= x; k++) {
                        v[k] = ctx->rpl[k];
                    }
                    break;
                default:
                    goto bad_op;
            }
            break;
    }
    return 0;

bad_op:
    ctx->err = ERR_OPCODE;
    ctx->err_addr = pc;
    ctx->pc = pc;
    return 1;
stack_err:
    ctx->err = ERR_STACK;
    ctx->err_addr = pc;
    ctx->pc = pc;
    return 1;
}

// ---- core entry points, shared by the host harness and the native module -----

static int c8_init(ctx_t *ctx, const uint8_t *rom, uint32_t rom_len, uint32_t seed) {
    if (rom_len > MEM_SIZE - PROG_START) {
        return 1;
    }
    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->mem + FONT_SMALL, font_small, sizeof(font_small));
    memcpy(ctx->mem + FONT_BIG, font_big, sizeof(font_big));
    memcpy(ctx->mem + PROG_START, rom, rom_len);
    ctx->pc = PROG_START;
    ctx->wait_key = -1;
    ctx->quirks = QUIRKS_CHIP8;
    ctx->ipf = 11;
    ctx->rng = seed ? seed : 0x2545F491;
    ctx->fg = 0xFFFF;
    ctx->bg = 0x0000;
    ctx->half_period = AUDIO_RATE / (2 * 440);
    ctx->amp = 4000;
    ctx->dirty = 1;
    return 0;
}

static void c8_audio_frame(ctx_t *ctx) {
    int16_t *ring = ctx->ring;
    const uint32_t frames = ctx->ring_frames;
    uint32_t pos = ctx->ring_pos;
    const int beeping = ctx->st > 0;
    for (uint32_t k = 0; k < AUDIO_SAMPLES; k++) {
        int16_t s = 0;
        if (beeping) {
            s = (int16_t)(ctx->phase < ctx->half_period ? ctx->amp : -ctx->amp);
            if (++ctx->phase >= 2 * ctx->half_period) {
                ctx->phase = 0;
            }
        }
        ring[pos * 2] = s;
        ring[pos * 2 + 1] = s;
        if (++pos == frames) {
            pos = 0;
        }
    }
    ctx->ring_pos = pos;
}

static uint32_t c8_run_frames(ctx_t *ctx, uint32_t n) {
    uint32_t f = 0;
    while (f < n && ctx->err == ERR_NONE) {
        for (uint32_t k = 0; k < ctx->ipf; k++) {
            if (step(ctx)) {
                break;
            }
        }
        if (ctx->ring != NULL) {
            c8_audio_frame(ctx);
        }
        if (ctx->dt) {
            ctx->dt--;
        }
        if (ctx->st) {
            ctx->st--;
        }
        if (ctx->fb != NULL && ctx->dirty) {
            render(ctx);
            ctx->dirty = 0;
        }
        f++;
    }
    return f;
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

// init(st, rom, seed) -> 0, or 1 if the ROM does not fit in 3584 bytes.
static mp_obj_t mod_init(mp_obj_t st_in, mp_obj_t rom_in, mp_obj_t seed_in) {
    ctx_t *ctx = get_ctx(st_in);
    mp_buffer_info_t rom;
    mp_get_buffer_raise(rom_in, &rom, MP_BUFFER_READ);
    return mp_obj_new_int(c8_init(ctx, rom.buf, rom.len, mp_obj_get_int_truncated(seed_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_init_obj, mod_init);

static mp_obj_t mod_config(mp_obj_t st_in, mp_obj_t quirks_in, mp_obj_t ipf_in) {
    ctx_t *ctx = get_ctx(st_in);
    mp_int_t ipf = mp_obj_get_int(ipf_in);
    if (ipf < 1 || ipf > 100000) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad ipf"));
    }
    ctx->quirks = mp_obj_get_int(quirks_in);
    ctx->ipf = ipf;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_config_obj, mod_config);

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
    if ((bpp != 8 && bpp != 16) || width < 64 || height < 32 ||
        width * height * (bpp / 8) > fb.len) {
        mp_raise_ValueError(MP_ERROR_TEXT("output does not fit"));
    }
    ctx->fb = fb.buf;
    ctx->fb_w = width;
    ctx->fb_h = height;
    ctx->bpp = bpp;
    ctx->shown = 0;
    ctx->dirty = 1;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_set_output_obj, 5, 5, mod_set_output);

static mp_obj_t mod_set_colors(mp_obj_t st_in, mp_obj_t fg_in, mp_obj_t bg_in) {
    ctx_t *ctx = get_ctx(st_in);
    ctx->fg = mp_obj_get_int(fg_in);
    ctx->bg = mp_obj_get_int(bg_in);
    ctx->shown = 0;
    ctx->dirty = 1;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_set_colors_obj, mod_set_colors);

static mp_obj_t mod_set_keys(mp_obj_t st_in, mp_obj_t mask_in) {
    get_ctx(st_in)->keys = mp_obj_get_int(mask_in) & 0xFFFF;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_set_keys_obj, mod_set_keys);

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

// set_beep(st, hz, amplitude): amplitude 0..32767.
static mp_obj_t mod_set_beep(mp_obj_t st_in, mp_obj_t hz_in, mp_obj_t amp_in) {
    ctx_t *ctx = get_ctx(st_in);
    mp_int_t hz = mp_obj_get_int(hz_in);
    mp_int_t amp = mp_obj_get_int(amp_in);
    if (hz < 20 || hz > AUDIO_RATE / 2 || amp < 0 || amp > 32767) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad beep"));
    }
    ctx->half_period = AUDIO_RATE / (2 * hz);
    ctx->amp = amp;
    ctx->phase = 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_set_beep_obj, mod_set_beep);

static mp_obj_t mod_run_frames(mp_obj_t st_in, mp_obj_t n_in) {
    return mp_obj_new_int(c8_run_frames(get_ctx(st_in), mp_obj_get_int(n_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_run_frames_obj, mod_run_frames);

static mp_obj_t mod_frame_crc(mp_obj_t st_in) {
    ctx_t *ctx = get_ctx(st_in);
    return mp_obj_new_int_from_uint(crc32(ctx->disp, sizeof(ctx->disp)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_frame_crc_obj, mod_frame_crc);

// error(st) -> (code, addr); code is ERR_*, 0 means running.
static mp_obj_t mod_error(mp_obj_t st_in) {
    ctx_t *ctx = get_ctx(st_in);
    mp_obj_t t[2] = {mp_obj_new_int(ctx->err), mp_obj_new_int(ctx->err_addr)};
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_error_obj, mod_error);

mp_obj_t mpy_init(mp_obj_fun_bc_t *self, size_t n_args, size_t n_kw, mp_obj_t *args) {
    MP_DYNRUNTIME_INIT_ENTRY
    mp_store_global(MP_QSTR_STATE_SIZE, MP_OBJ_NEW_SMALL_INT(sizeof(ctx_t)));
    mp_store_global(MP_QSTR_AUDIO_RATE, MP_OBJ_NEW_SMALL_INT(AUDIO_RATE));
    mp_store_global(MP_QSTR_AUDIO_SAMPLES_PER_FRAME, MP_OBJ_NEW_SMALL_INT(AUDIO_SAMPLES));
    mp_store_global(MP_QSTR_QUIRK_VF_RESET, MP_OBJ_NEW_SMALL_INT(QUIRK_VF_RESET));
    mp_store_global(MP_QSTR_QUIRK_MEMORY, MP_OBJ_NEW_SMALL_INT(QUIRK_MEMORY));
    mp_store_global(MP_QSTR_QUIRK_DISPLAY_WAIT, MP_OBJ_NEW_SMALL_INT(QUIRK_DISPLAY_WAIT));
    mp_store_global(MP_QSTR_QUIRK_CLIPPING, MP_OBJ_NEW_SMALL_INT(QUIRK_CLIPPING));
    mp_store_global(MP_QSTR_QUIRK_SHIFTING, MP_OBJ_NEW_SMALL_INT(QUIRK_SHIFTING));
    mp_store_global(MP_QSTR_QUIRK_JUMPING, MP_OBJ_NEW_SMALL_INT(QUIRK_JUMPING));
    mp_store_global(MP_QSTR_QUIRKS_CHIP8, MP_OBJ_NEW_SMALL_INT(QUIRKS_CHIP8));
    mp_store_global(MP_QSTR_QUIRKS_SCHIP, MP_OBJ_NEW_SMALL_INT(QUIRKS_SCHIP));
    mp_store_global(MP_QSTR_ERR_OPCODE, MP_OBJ_NEW_SMALL_INT(ERR_OPCODE));
    mp_store_global(MP_QSTR_ERR_STACK, MP_OBJ_NEW_SMALL_INT(ERR_STACK));
    mp_store_global(MP_QSTR_ERR_EXIT, MP_OBJ_NEW_SMALL_INT(ERR_EXIT));
    mp_store_global(MP_QSTR_init, MP_OBJ_FROM_PTR(&mod_init_obj));
    mp_store_global(MP_QSTR_config, MP_OBJ_FROM_PTR(&mod_config_obj));
    mp_store_global(MP_QSTR_set_output, MP_OBJ_FROM_PTR(&mod_set_output_obj));
    mp_store_global(MP_QSTR_set_colors, MP_OBJ_FROM_PTR(&mod_set_colors_obj));
    mp_store_global(MP_QSTR_set_keys, MP_OBJ_FROM_PTR(&mod_set_keys_obj));
    mp_store_global(MP_QSTR_set_audio, MP_OBJ_FROM_PTR(&mod_set_audio_obj));
    mp_store_global(MP_QSTR_set_beep, MP_OBJ_FROM_PTR(&mod_set_beep_obj));
    mp_store_global(MP_QSTR_run_frames, MP_OBJ_FROM_PTR(&mod_run_frames_obj));
    mp_store_global(MP_QSTR_frame_crc, MP_OBJ_FROM_PTR(&mod_frame_crc_obj));
    mp_store_global(MP_QSTR_error, MP_OBJ_FROM_PTR(&mod_error_obj));
    MP_DYNRUNTIME_INIT_EXIT
}

#endif
