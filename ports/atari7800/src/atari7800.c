// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Native module `atari7800`: an Atari 7800 (6502C "Sally" + MARIA + TIA sound
// + RIOT, optional POKEY) written for this project, as a CircuitPython
// native .mpy. MARIA behavior (line RAM cells, display list parsing, DMA
// cycle costs, DLI timing) follows MAME's maria.cpp (BSD-3-Clause, see
// LICENSE.mame); the code here is new.
//
// Every buffer lives in a Python-owned bytearray and is passed in, so the GC
// sees all of it and nothing has to be allocated from native code:
//
//   st = bytearray(atari7800.STATE_SIZE)        # whole machine, keep alive
//   atari7800.init(st, rom)                     # rom: whole .a78 or .bin file, keep alive
//                                               # -> 0 ok, 1 bad size, 2 unsupported mapper
//   atari7800.info(st)                          # -> (mapper name, rom bytes, pokey, pal)
//   atari7800.set_output(st, fb, width, height, bpp, top)   # optional
//   atari7800.set_input(st, mask)               # INPUT_* bits below, 1 = pressed/on
//   atari7800.run_frames(st, n)                 # -> frames run, stops on a JAM opcode
//   atari7800.set_audio(st, ring, start)        # optional, see below
//   atari7800.samples(st)                       # audio samples made since init
//   atari7800.frame_crc(st, enable)             # CRC-32 of the last frame (if enabled)
//
// Video: MARIA's 160 cell line is drawn as 320 framebuffer pixels (160 modes
// 2x wide, 320 modes 1:1), so width must be at least 320. Scanline `top`
// lands on framebuffer row 0; `height` rows are drawn. bpp 16 is RGB565,
// bpp 8 is RGB332.
//
// Sound: TIA (and POKEY when the cart has one) mixed to mono int16 at
// AUDIO_RATE. With a ring set, samples are appended as they are made,
// wrapping. Make the ring the buffer of a looping, double buffered
// audiocore.RawSample and start writing one half ahead of playback.
//
// Timing model: time is counted in MARIA clocks (7.16 MHz), 454 per line.
// A CPU cycle is 4 clocks, 6 when it touches TIA or RIOT. Each line: MARIA
// starts DMA 28 clocks in, builds line RAM for the next line in one go and
// holds the CPU for the DMA time; the line RAM built on the previous line is
// turned into pixels at the same moment. WSYNC holds the CPU to the end of
// the line. No BIOS: the machine starts at the cart reset vector with MARIA
// DMA off, as the BIOS leaves it.

#ifdef HOST_TEST
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#else
#include "py/dynruntime.h"

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

#include "palette.h"

#define AUDIO_RATE 22050
#define CLK_NTSC 7159090      // MARIA clock, 14.31818 MHz / 2
#define CLK_PAL 7093788
#define LINE_CLK 454
#define TICK_CLK 227          // two audio ticks per line (~31.5 kHz)
#define DMA_AT 28             // DMA starts 7 CPU cycles into the line
#define DMA_MAX 426           // MARIA clocks available for DMA

// set_input() bits
#define IN_UP 0x001
#define IN_DOWN 0x002
#define IN_LEFT 0x004
#define IN_RIGHT 0x008
#define IN_FIRE1 0x010      // left button (button 1)
#define IN_FIRE2 0x020      // right button (button 2)
#define IN_RESET 0x040
#define IN_SELECT 0x080
#define IN_PAUSE 0x100
#define IN_LEFT_A 0x200     // left difficulty set to A
#define IN_RIGHT_A 0x400    // right difficulty set to A

enum { CT_FLAT, CT_SG, CT_SGRAM, CT_SG9, CT_MRAM };

typedef struct {            // TIA audio channel
    uint8_t c, f, v;
    uint8_t div, out, d31, d3, p4, p5;
    uint16_t p9;
} chan_t;

typedef struct {            // POKEY channel
    uint8_t f, c;
    uint8_t out, hp;
    int32_t cnt;            // clocks from the start of this tick to the next pulse
} pchan_t;

typedef struct {
    // ---- cartridge ----
    const uint8_t *rom;
    uint32_t rom_len;
    uint32_t cart_type, nbanks, bank;
    uint32_t pokey4000, pokey450, pal_tv, mapper;
    const uint8_t *rpage[256];     // readable pages (NULL: slow path)
    uint8_t *wpage[256];           // writable pages (NULL: slow path)

    // ---- CPU ----
    uint8_t A, X, Y, S;
    uint16_t PC;
    uint8_t jammed, nmi, wsync;
    uint32_t C_, V_, D_, I_, N_, Z_;
    uint32_t clk;                  // MARIA clocks since the start of the frame
    uint32_t xclk;                 // extra clocks for slow (TIA/RIOT) accesses

    // ---- MARIA ----
    uint8_t mpal[32];              // color registers; entries 0, 4, .. 28 are BACKGRND
    uint8_t dmaon, cwidth, kangaroo, rm, colorkill, wm, vblank;
    uint8_t offset, holey, dli, charbase;
    uint16_t dpp, dll, dl;
    uint8_t active;
    uint8_t lineram[2][160];
    uint32_t line, nlines;

    // ---- RIOT ----
    uint8_t swcha_in, swchb_in, swacnt, swbcnt, swcha_out, swchb_out;
    uint8_t tim_start, tim_shift, tim_ack, buttons;
    int32_t tim_base;              // clock the timer was written (end of write)
    uint32_t pad_;

    // ---- TIA audio ----
    chan_t ch[2];

    // ---- POKEY ----
    pchan_t pk[4];
    uint8_t audctl, skctl;
    uint32_t pk_per[4];            // pulse periods, see pokey_period()
    uint32_t pk_clk;               // MARIA clocks, wraps at POLY_WRAP
    uint32_t lfsr17;
    uint8_t poly4[15], poly5[31], poly9[511];

    // ---- audio ----
    uint32_t aacc, asum, an, clock_hz, level_scale;
    uint32_t nsamples;
    int16_t *ring;
    uint32_t ring_len, ring_pos;

    // ---- output ----
    uint8_t *fb;
    uint32_t fb_width, fb_height, bpp, x0, top;
    uint32_t crc_on, crc, frame_crc;
    uint16_t pal16[256];
    uint8_t pal8[256];
    uint32_t frames;

    // ---- memory ----
    uint8_t ram[0x1000];           // $1800-$27FF; $2040-$20FF and $2140-$21FF also at $0040/$0140
    uint8_t riot_ram[128];
    uint8_t cart_ram[0x4000];
} ctx_t;

#define POLY_WRAP (15 * 31 * 511 * 4)

// ============================================================================
// Cartridge

static const char *mapper_name(ctx_t *ctx) {
    switch (ctx->cart_type) {
        case CT_SG: return "SG";
        case CT_SGRAM: return "SG+RAM";
        case CT_SG9: return "SG9";
        case CT_MRAM: return "flat+RAM";
        default: return "flat";
    }
}

static void map_bank(ctx_t *ctx, uint32_t region, uint32_t bank) {
    // region: 0 = $4000, 1 = $8000, 2 = $C000; 16K ROM bank
    const uint8_t *p = ctx->rom + bank * 0x4000;
    for (uint32_t i = 0; i < 64; i++) {
        ctx->rpage[0x40 + region * 64 + i] = p + i * 256;
    }
}

static void map_cart(ctx_t *ctx) {
    for (uint32_t p = 0x40; p < 0x100; p++) {
        ctx->rpage[p] = NULL;
        ctx->wpage[p] = NULL;
    }
    if (ctx->cart_type == CT_FLAT || ctx->cart_type == CT_MRAM) {
        uint32_t base = 0x10000 - ctx->rom_len;
        for (uint32_t p = 0x40; p < 0x100; p++) {
            if ((p << 8) >= base) {
                ctx->rpage[p] = ctx->rom + (p << 8) - base;
            }
        }
        if (ctx->cart_type == CT_MRAM) {
            // 16K RAM at $4000 with A8 not connected: odd pages mirror even ones.
            for (uint32_t p = 0x40; p < 0x80; p++) {
                uint8_t *r = ctx->cart_ram + (((p - 0x40) & ~1u) << 8);
                ctx->rpage[p] = r;
                ctx->wpage[p] = r;
            }
        }
    } else {
        uint32_t nb = ctx->nbanks;
        map_bank(ctx, 2, nb - 1);
        map_bank(ctx, 1, ctx->bank);
        if (ctx->cart_type == CT_SGRAM) {
            for (uint32_t i = 0; i < 64; i++) {
                ctx->rpage[0x40 + i] = ctx->wpage[0x40 + i] = ctx->cart_ram + i * 256;
            }
        } else if (ctx->cart_type == CT_SG9) {
            map_bank(ctx, 0, 0);
        } else if (nb >= 2) {
            map_bank(ctx, 0, nb - 2);   // MAME's choice: second to last bank
        }
    }
    if (ctx->pokey4000) {
        for (uint32_t p = 0x40; p < 0x80; p++) {
            ctx->rpage[p] = NULL;
            ctx->wpage[p] = NULL;
        }
    }
}

static void cart_bank_wr(ctx_t *ctx, uint32_t a, uint8_t v) {
    if (ctx->cart_type == CT_FLAT || ctx->cart_type == CT_MRAM || a < 0x8000 || a >= 0xC000) {
        return;
    }
    uint32_t nb = ctx->nbanks;
    uint32_t b;
    if (ctx->cart_type == CT_SG9) {
        b = (v % (nb - 1)) + 1;
    } else {
        b = (nb & (nb - 1)) ? v % nb : (v & (nb - 1));
    }
    if (b != ctx->bank) {
        ctx->bank = b;
        map_bank(ctx, 1, b);
    }
}

// Parse the .a78 header (or guess from the size) and set up the mapper.
static int cart_setup(ctx_t *ctx, const uint8_t *data, uint32_t len) {
    uint32_t mapper = 0, pal = 0;
    int has_header = len >= 128 && data[1] == 'A' && data[2] == 'T' && data[3] == 'A' &&
        data[4] == 'R' && data[5] == 'I' && data[6] == '7' && data[7] == '8';
    if (has_header) {
        uint32_t n = ((uint32_t)data[49] << 24) | ((uint32_t)data[50] << 16) | (data[51] << 8) | data[52];
        mapper = (data[53] << 8) | data[54];
        pal = data[57] & 1;
        data += 128;
        len -= 128;
        if (n != 0 && n < len) {
            len = n;
        }
    }
    if (len < 0x1000 || len > 0x100000) {
        return 1;
    }
    if (!has_header) {
        if (len <= 0xC000) {
            mapper = 0;
        } else if (len == 0x24000) {
            mapper = 0x0A;
        } else {
            mapper = 0x02;
        }
    }
    ctx->mapper = mapper;
    ctx->pal_tv = pal;
    if (mapper & 0xFF00) {
        return 2;       // Activision, Absolute, banksets, souper, ... not supported
    }
    ctx->pokey450 = (mapper & 0x40) != 0;
    switch (mapper & 0x2E) {
        case 0x00: ctx->cart_type = CT_FLAT; break;
        case 0x02: ctx->cart_type = CT_SG; break;
        case 0x06: ctx->cart_type = CT_SGRAM; break;
        case 0x0A: ctx->cart_type = CT_SG9; break;
        case 0x22: case 0x26: ctx->cart_type = CT_SGRAM; break;   // VersaBoard: treated as SG+RAM
        default: return 2;
    }
    if (mapper & 0x80) {
        ctx->cart_type = CT_MRAM;
    }
    ctx->pokey4000 = (mapper & 1) && (ctx->cart_type == CT_FLAT || ctx->cart_type == CT_SG);
    if (ctx->cart_type == CT_FLAT || ctx->cart_type == CT_MRAM) {
        if (len > 0xC000) {
            data += len - 0xC000;
            len = 0xC000;
        }
    } else {
        ctx->nbanks = len / 0x4000;
        if (ctx->nbanks < 2 || (ctx->cart_type == CT_SG9 && ctx->nbanks < 3)) {
            return 1;
        }
    }
    ctx->rom = data;
    ctx->rom_len = len;
    ctx->bank = ctx->cart_type == CT_SG9 ? 1 : 0;
    map_cart(ctx);
    return 0;
}

// ============================================================================
// Memory read without side effects, used by MARIA DMA.

static uint8_t peek(ctx_t *ctx, uint32_t a) {
    a &= 0xFFFF;
    const uint8_t *p = ctx->rpage[a >> 8];
    if (p) {
        return p[a & 0xFF];
    }
    if (a < 0x200 && (a & 0xC0)) {
        return ctx->ram[0x800 + a];
    }
    return 0;
}

// ============================================================================
// MARIA

static inline int is_holey(ctx_t *ctx, uint32_t a) {
    return ((ctx->holey & 2) && (a & 0x9000) == 0x9000) || ((ctx->holey & 1) && (a & 0x8800) == 0x8800);
}

// One graphics byte into line RAM at cell hpos; returns the next cell.
static inline uint32_t write_cells(ctx_t *ctx, uint8_t *lr, uint32_t d, uint32_t hpos, uint32_t pal) {
    uint32_t keep = ctx->kangaroo;
    if (ctx->wm) {
        // 160B / 320B / 320C: two cells, P2 + two data bits + two color bits.
        if (!keep) {
            if (!(d & 0xF0)) {
                return (hpos + 2) & 0xFF;
            }
            if (hpos <= 158) {
                if (d & 0xC0) lr[hpos] = (uint8_t)((pal & 0x10) | (d & 0x0C) | (d >> 6));
                if (d & 0x30) lr[hpos + 1] = (uint8_t)((pal & 0x10) | ((d & 0x03) << 2) | ((d & 0x30) >> 4));
                return hpos + 2;
            }
        }
        uint32_t c = (pal & 0x10) | (d & 0x0C) | (d >> 6);
        if (((c & 3) || keep) && hpos < 160) {
            lr[hpos] = (uint8_t)c;
        }
        hpos = (hpos + 1) & 0xFF;
        c = (pal & 0x10) | ((d & 0x03) << 2) | ((d & 0x30) >> 4);
        if (((c & 3) || keep) && hpos < 160) {
            lr[hpos] = (uint8_t)c;
        }
        return (hpos + 1) & 0xFF;
    }
    // 160A / 320A / 320D: four cells, palette + two bits.
    if (!keep) {
        if (d == 0) {
            return (hpos + 4) & 0xFF;
        }
        if (hpos <= 156) {
            uint8_t *q = lr + hpos;
            if (d & 0xC0) q[0] = (uint8_t)(pal | (d >> 6));
            if (d & 0x30) q[1] = (uint8_t)(pal | ((d >> 4) & 3));
            if (d & 0x0C) q[2] = (uint8_t)(pal | ((d >> 2) & 3));
            if (d & 0x03) q[3] = (uint8_t)(pal | (d & 3));
            return hpos + 4;
        }
    }
    for (int i = 6; i >= 0; i -= 2) {
        uint32_t c = pal | ((d >> i) & 3);
        if (((c & 3) || keep) && hpos < 160) {
            lr[hpos] = (uint8_t)c;
        }
        hpos = (hpos + 1) & 0xFF;
    }
    return hpos;
}

static void load_dll_entry(ctx_t *ctx) {
    uint8_t h = peek(ctx, ctx->dll);
    ctx->dl = (uint16_t)((peek(ctx, ctx->dll + 1) << 8) | peek(ctx, ctx->dll + 2));
    ctx->offset = h & 0x0F;
    ctx->holey = (h >> 5) & 3;
    ctx->dli = h >> 7;
}

// Build line RAM for the next line from the current zone. Returns DMA clocks.
static uint32_t maria_dma(ctx_t *ctx) {
    uint32_t mc = 16;
    uint8_t *lr = ctx->lineram[ctx->active];
    uint32_t dl = ctx->dl;
    uint32_t off8 = (uint32_t)ctx->offset << 8;
    while (mc < DMA_MAX) {
        uint32_t m = peek(ctx, dl + 1);
        if (!(m & 0x5F)) {
            break;
        }
        uint32_t lo = peek(ctx, dl), hi = peek(ctx, dl + 2), pw, hpos, ind;
        if (!(m & 0x1F)) {          // 5 byte extended header
            pw = peek(ctx, dl + 3);
            hpos = peek(ctx, dl + 4);
            ctx->wm = m >> 7;
            ind = m & 0x20;
            dl += 5;
            mc += 10;
        } else {
            pw = m;
            hpos = peek(ctx, dl + 3);
            ind = 0;
            dl += 4;
            mc += 8;
        }
        uint32_t width = ((pw ^ 0xFF) & 0x1F) + 1;
        uint32_t pal = (pw >> 5) << 2;
        uint32_t ga = (hi << 8) | lo;
        if (ind) {
            const uint8_t *cm = ctx->rpage[ga >> 8];
            uint32_t gpage = (ctx->charbase + ctx->offset) & 0xFF;
            const uint8_t *gp = ctx->rpage[gpage];
            if (cm != NULL && gp != NULL && (ga & 0xFF) + width <= 256) {
                // Char map in one page, all glyph rows in one page.
                if (is_holey(ctx, gpage << 8)) {
                    continue;
                }
                uint32_t step = ctx->cwidth ? 9 : 6;
                uint32_t n = mc < DMA_MAX ? (DMA_MAX - mc + step - 1) / step : 0;
                if (n > width) {
                    n = width;
                }
                cm += ga & 0xFF;
                for (uint32_t x = 0; x < n; x++) {
                    uint32_t c = cm[x];
                    hpos = write_cells(ctx, lr, gp[c], hpos, pal);
                    if (step == 9) {
                        uint32_t d = c < 255 ? gp[c + 1] : peek(ctx, ((gpage << 8) + 256) & 0xFFFF);
                        hpos = write_cells(ctx, lr, d, hpos, pal);
                    }
                }
                mc += step * n;
                continue;
            }
            for (uint32_t x = 0; x < width && mc < DMA_MAX; x++) {
                uint32_t c = peek(ctx, ga + x);
                uint32_t da = ((((uint32_t)ctx->charbase << 8) | c) + off8) & 0xFFFF;
                if (is_holey(ctx, da)) {
                    continue;
                }
                hpos = write_cells(ctx, lr, peek(ctx, da), hpos, pal);
                if (ctx->cwidth) {
                    hpos = write_cells(ctx, lr, peek(ctx, da + 1), hpos, pal);
                    mc += 9;
                } else {
                    mc += 6;
                }
            }
            continue;
        }
        uint32_t da = (ga + off8) & 0xFFFF;
        const uint8_t *p = ctx->rpage[da >> 8];
        if (p != NULL && (da & 0xFF) + width <= 256) {
            // Whole object in one page: holey DMA is decided per 2K/4K
            // block, so one check covers it.
            if (is_holey(ctx, da)) {
                continue;
            }
            uint32_t n = mc < DMA_MAX ? (DMA_MAX - mc + 2) / 3 : 0;
            if (n > width) {
                n = width;
            }
            p += da & 0xFF;
            for (uint32_t x = 0; x < n; x++) {
                hpos = write_cells(ctx, lr, p[x], hpos, pal);
            }
            mc += 3 * n;
            continue;
        }
        for (uint32_t x = 0; x < width && mc < DMA_MAX; x++) {
            da = (ga + x + off8) & 0xFFFF;
            if (is_holey(ctx, da)) {
                continue;
            }
            hpos = write_cells(ctx, lr, peek(ctx, da), hpos, pal);
            mc += 3;
        }
    }
    if (ctx->offset == 0) {
        mc += 6;
        if (peek(ctx, ctx->dll + 3) & 0x80) {
            mc += 17;
        }
    }
    return mc;
}

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n) {
    static const uint32_t t[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    while (n--) {
        c ^= *p++;
        c = (c >> 4) ^ t[c & 15];
        c = (c >> 4) ^ t[c & 15];
    }
    return c;
}

// Line RAM cell -> palette slot for the left and right pixel of the cell.
static inline uint32_t slot_l(uint32_t rm, uint32_t c) {
    if (rm == 2) {
        return (c & 0x10) | (c & 0x02) | ((c >> 3) & 1);
    }
    if (rm == 3) {
        return (c & 0x1C) | (c & 0x02);
    }
    return c;
}
static inline uint32_t slot_r(uint32_t rm, uint32_t c) {
    if (rm == 2) {
        return (c & 0x10) | ((c << 1) & 0x02) | ((c >> 2) & 1);
    }
    if (rm == 3) {
        return (c & 0x1C) | ((c << 1) & 0x02);
    }
    return c;
}

// Show the line RAM built on the previous line, then clear it.
static void display_line(ctx_t *ctx) {
    ctx->active ^= 1;
    uint8_t *cells = ctx->lineram[ctx->active];
    uint32_t row = ctx->line - ctx->top;
    uint32_t rm = ctx->rm;
    if (row < ctx->fb_height && (ctx->fb != NULL || ctx->crc_on)) {
        uint8_t col[32];
        for (int i = 0; i < 32; i++) {
            col[i] = ctx->colorkill ? (ctx->mpal[i] & 0x0F) : ctx->mpal[i];
        }
        if (ctx->crc_on) {
            uint8_t px[320];
            for (int x = 0; x < 160; x++) {
                px[2 * x] = col[slot_l(rm, cells[x])];
                px[2 * x + 1] = col[slot_r(rm, cells[x])];
            }
            if (row == 0) {
                ctx->crc = 0xFFFFFFFF;
            }
            ctx->crc = crc32_update(ctx->crc, px, 320);
            if (row == ctx->fb_height - 1) {
                ctx->frame_crc = ~ctx->crc;
            }
        }
        if (ctx->fb != NULL) {
            if (ctx->bpp == 16) {
                uint16_t c16[32];
                for (int i = 0; i < 32; i++) {
                    c16[i] = ctx->pal16[col[i]];
                }
                uint16_t *d = (uint16_t *)ctx->fb + row * ctx->fb_width + ctx->x0;
                if (rm < 2) {
                    uint32_t *d32 = (uint32_t *)d;
                    for (int x = 0; x < 160; x++) {
                        d32[x] = c16[cells[x]] * 0x10001u;
                    }
                } else {
                    for (int x = 0; x < 160; x++) {
                        d[2 * x] = c16[slot_l(rm, cells[x])];
                        d[2 * x + 1] = c16[slot_r(rm, cells[x])];
                    }
                }
            } else {
                uint8_t c8[32];
                for (int i = 0; i < 32; i++) {
                    c8[i] = ctx->pal8[col[i]];
                }
                uint8_t *d = ctx->fb + row * ctx->fb_width + ctx->x0;
                for (int x = 0; x < 160; x++) {
                    d[2 * x] = c8[slot_l(rm, cells[x])];
                    d[2 * x + 1] = c8[slot_r(rm, cells[x])];
                }
            }
        }
    }
    memset(cells, 0, 160);
}

// Rows of the output window that MARIA never draws (outside its display
// lines) are cleared to black once per frame.
static void blank_row(ctx_t *ctx) {
    uint32_t row = ctx->line - ctx->top;
    if (row < ctx->fb_height && ctx->fb != NULL) {
        uint32_t bytes = ctx->bpp / 8;
        memset(ctx->fb + (row * ctx->fb_width + ctx->x0) * bytes, 0, 320 * bytes);
    }
}

static void maria_wr(ctx_t *ctx, uint32_t r, uint8_t v) {
    if (r & 3) {
        ctx->mpal[r] = v;
        return;
    }
    switch (r) {
        case 0x00:
            for (int i = 0; i < 32; i += 4) {
                ctx->mpal[i] = v;
            }
            break;
        case 0x04:
            ctx->wsync = 1;
            break;
        case 0x0C:
            ctx->dpp = (uint16_t)((ctx->dpp & 0x00FF) | (v << 8));
            break;
        case 0x10:
            ctx->dpp = (uint16_t)((ctx->dpp & 0xFF00) | v);
            break;
        case 0x14:
            ctx->charbase = v;
            break;
        case 0x1C:
            ctx->colorkill = v >> 7;
            if (((v >> 5) & 3) == 2) {
                ctx->dmaon = 1;
            } else if (((v >> 5) & 3) == 3) {
                ctx->dmaon = 0;
            }
            ctx->cwidth = (v >> 4) & 1;
            ctx->kangaroo = (v >> 2) & 1;
            ctx->rm = v & 3;
            break;
    }
}

// ============================================================================
// TIA sound (same channel model as ../atari2600) and input ports

static void chan_clock(chan_t *ch) {
#define STEP4() (ch->p4 = (uint8_t)(((ch->p4 << 1) | (((ch->p4 >> 3) ^ (ch->p4 >> 2)) & 1)) & 0x0F))
#define STEP5() (ch->p5 = (uint8_t)(((ch->p5 << 1) | (((ch->p5 >> 4) ^ (ch->p5 >> 2)) & 1)) & 0x1F))
#define STEP9() (ch->p9 = (uint16_t)(((ch->p9 << 1) | (((ch->p9 >> 8) ^ (ch->p9 >> 4)) & 1)) & 0x1FF))
    uint32_t t31;
    switch (ch->c) {
        case 0x0: case 0xB:
            ch->out = 1;
            break;
        case 0x1:
            STEP4();
            ch->out = ch->p4 & 1;
            break;
        case 0x2:
            if (++ch->d31 >= 31) {
                ch->d31 = 0;
            }
            if (ch->d31 == 0 || ch->d31 == 18) {
                STEP4();
            }
            ch->out = ch->p4 & 1;
            break;
        case 0x3:
            STEP5();
            if (ch->p5 & 1) {
                STEP4();
            }
            ch->out = ch->p4 & 1;
            break;
        case 0x4: case 0x5:
            ch->out ^= 1;
            break;
        case 0x6: case 0xA:
            if (++ch->d31 >= 31) {
                ch->d31 = 0;
            }
            ch->out = ch->d31 < 18;
            break;
        case 0x7:
            STEP5();
            if (ch->p5 & 1) {
                ch->out ^= 1;
            }
            break;
        case 0x8:
            STEP9();
            ch->out = ch->p9 & 1;
            break;
        case 0x9:
            STEP5();
            ch->out = ch->p5 & 1;
            break;
        case 0xC: case 0xD:
            if (++ch->d3 >= 3) {
                ch->d3 = 0;
                ch->out ^= 1;
            }
            break;
        case 0xE:
            if (++ch->d31 >= 31) {
                ch->d31 = 0;
            }
            t31 = ch->d31 == 0 || ch->d31 == 18;
            if (t31 && ++ch->d3 >= 3) {
                ch->d3 = 0;
                ch->out ^= 1;
            }
            break;
        case 0xF:
            STEP5();
            if ((ch->p5 & 1) && ++ch->d3 >= 3) {
                ch->d3 = 0;
                ch->out ^= 1;
            }
            break;
    }
#undef STEP4
#undef STEP5
#undef STEP9
}

static uint8_t tia_rd(ctx_t *ctx, uint32_t r) {
    uint32_t b = ctx->buttons;
    // Pin 6 is only pulled up (one button mode) unless SWCHB bit 2 drives it low.
    uint32_t one_button = !((ctx->swbcnt & 4) && !(ctx->swchb_out & 4));
    switch (r & 0x0F) {
        case 0x08: return (b & 2) ? 0x80 : 0;          // INPT0: P0 right button
        case 0x09: return (b & 1) ? 0x80 : 0;          // INPT1: P0 left button
        case 0x0C: return (b && one_button) ? 0 : 0x80; // INPT4
        case 0x0D: return 0x80;
        default: return 0;
    }
}

static void tia_wr(ctx_t *ctx, uint32_t r, uint8_t v) {
    switch (r) {
        case 0x15: ctx->ch[0].c = v & 0x0F; break;
        case 0x16: ctx->ch[1].c = v & 0x0F; break;
        case 0x17: ctx->ch[0].f = v & 0x1F; break;
        case 0x18: ctx->ch[1].f = v & 0x1F; break;
        case 0x19: ctx->ch[0].v = v & 0x0F; break;
        case 0x1A: ctx->ch[1].v = v & 0x0F; break;
    }
}

// ============================================================================
// POKEY: four channels with the 4/5/9/17 bit polynomial counters. Written
// from the documented register behavior; timing is per channel pulse, with
// output high time integrated over each audio tick.

static uint32_t pokey_period(ctx_t *ctx, int i);

static void pokey_init(ctx_t *ctx) {
    uint32_t s = 0xF;
    for (int i = 0; i < 15; i++) {
        ctx->poly4[i] = s & 1;
        s = ((s << 1) | (((s >> 3) ^ (s >> 2)) & 1)) & 0xF;
    }
    s = 0x1F;
    for (int i = 0; i < 31; i++) {
        ctx->poly5[i] = s & 1;
        s = ((s << 1) | (((s >> 4) ^ (s >> 2)) & 1)) & 0x1F;
    }
    s = 0x1FF;
    for (int i = 0; i < 511; i++) {
        ctx->poly9[i] = s & 1;
        s = ((s << 1) | (((s >> 8) ^ (s >> 3)) & 1)) & 0x1FF;
    }
    ctx->lfsr17 = 0x1FFFF;
    for (int i = 0; i < 4; i++) {
        ctx->pk_per[i] = pokey_period(ctx, i);
    }
}

static inline uint32_t step17(ctx_t *ctx) {
    uint32_t s = ctx->lfsr17;
    s = ((s << 1) | (((s >> 16) ^ (s >> 11)) & 1)) & 0x1FFFF;
    ctx->lfsr17 = s;
    return s & 1;
}

// Pulse period of channel i in MARIA clocks (4 per CPU cycle). 0 = silent
// low half of a 16 bit pair.
static uint32_t pokey_period(ctx_t *ctx, int i) {
    uint32_t ac = ctx->audctl;
    uint32_t base = (ac & 1) ? 114 * 4 : 28 * 4;
    pchan_t *p = ctx->pk;
    switch (i) {
        case 0:
            if (ac & 0x10) return 0;
            return (ac & 0x40) ? (p[0].f + 4u) * 4 : (p[0].f + 1u) * base;
        case 1:
            if (ac & 0x10) {
                uint32_t f = (p[1].f << 8) | p[0].f;
                return (ac & 0x40) ? (f + 7) * 4 : (f + 1) * base;
            }
            return (p[1].f + 1u) * base;
        case 2:
            if (ac & 0x08) return 0;
            return (ac & 0x20) ? (p[2].f + 4u) * 4 : (p[2].f + 1u) * base;
        default:
            if (ac & 0x08) {
                uint32_t f = (p[3].f << 8) | p[2].f;
                return (ac & 0x20) ? (f + 7) * 4 : (f + 1) * base;
            }
            return (p[3].f + 1u) * base;
    }
}

// Advance POKEY by one audio tick; returns sum of volume x high clocks.
static uint32_t pokey_tick(ctx_t *ctx) {
    if (!(ctx->skctl & 3)) {
        return 0;
    }
    uint32_t total = 0;
    uint32_t ac = ctx->audctl;
    for (int i = 0; i < 4; i++) {
        pchan_t *ch = &ctx->pk[i];
        uint32_t per = ctx->pk_per[i];
        uint32_t vol = ch->c & 0x0F;
        int32_t t = ch->cnt;
        uint32_t hi = 0, last = 0;
        uint32_t hp = (i == 0 && (ac & 4)) || (i == 1 && (ac & 2));
        if (per != 0 && vol == 0 && !(i == 2 && (ac & 4)) && !(i == 3 && (ac & 2))) {
            // Silent and not clocking a high-pass filter: just keep the
            // divider phase.
            if (t > (int32_t)per) {
                t = (int32_t)per;
            }
            if (t < TICK_CLK) {
                t += (int32_t)(((TICK_CLK - (uint32_t)t + per - 1) / per) * per);
            }
            ch->cnt = t - TICK_CLK;
            continue;
        }
        if (per != 0) {
            if (t > (int32_t)per) {
                t = (int32_t)per;
            }
            while (t < TICK_CLK) {
                if ((ch->out ^ (hp ? ch->hp : 0)) & 1) {
                    hi += (uint32_t)t - last;
                }
                last = (uint32_t)t;
                uint32_t n = (ctx->pk_clk + (uint32_t)t) >> 2;   // CPU cycle of this pulse
                uint32_t c = ch->c;
                if ((c & 0x80) || ctx->poly5[n % 31]) {
                    if (c & 0x20) {
                        ch->out ^= 1;
                    } else if (c & 0x40) {
                        ch->out = ctx->poly4[n % 15];
                    } else if (ac & 0x80) {
                        ch->out = ctx->poly9[n % 511];
                    } else {
                        ch->out = (uint8_t)step17(ctx);
                    }
                }
                if (i == 2) {
                    ctx->pk[0].hp = ctx->pk[0].out;
                } else if (i == 3) {
                    ctx->pk[1].hp = ctx->pk[1].out;
                }
                t += (int32_t)per;
            }
            ch->cnt = t - TICK_CLK;
        }
        if ((ch->out ^ (hp ? ch->hp : 0)) & 1) {
            hi += TICK_CLK - last;
        }
        if (per == 0) {
            continue;
        }
        total += (ch->c & 0x10) ? vol * TICK_CLK : vol * hi;
    }
    ctx->pk_clk += TICK_CLK;
    if (ctx->pk_clk >= POLY_WRAP) {
        ctx->pk_clk -= POLY_WRAP;
    }
    return total;
}

static uint8_t pokey_rd(ctx_t *ctx, uint32_t r, uint32_t clk) {
    switch (r & 0x0F) {
        case 0x0A: {   // RANDOM: $FF while held in reset by SKCTL
            if (!(ctx->skctl & 3)) {
                return 0xFF;
            }
            for (int i = 0; i < 8; i++) {
                step17(ctx);
            }
            return (uint8_t)(ctx->lfsr17 ^ (clk >> 2));
        }
        case 0x0F: return 0xFF;   // SKSTAT: nothing pending
        default: return 0;
    }
}

static void pokey_wr(ctx_t *ctx, uint32_t r, uint8_t v) {
    r &= 0x0F;
    if (r < 8) {
        pchan_t *ch = &ctx->pk[r >> 1];
        if (r & 1) {
            ch->c = v;
        } else {
            ch->f = v;
        }
    } else if (r == 0x08) {
        ctx->audctl = v;
    } else if (r == 0x09) {   // STIMER: restart all dividers
        for (int i = 0; i < 4; i++) {
            ctx->pk[i].cnt = (int32_t)pokey_period(ctx, i);
        }
    } else if (r == 0x0F) {
        ctx->skctl = v;
    }
    for (int i = 0; i < 4; i++) {
        ctx->pk_per[i] = pokey_period(ctx, i);
    }
}

// Two audio ticks per line.
static void audio_line(ctx_t *ctx) {
    for (int k = 0; k < 2; k++) {
        uint32_t level = 0;
        for (int i = 0; i < 2; i++) {
            chan_t *ch = &ctx->ch[i];
            if (ch->div >= ch->f) {
                ch->div = 0;
                chan_clock(ch);
            } else {
                ch->div++;
            }
            level += ch->out ? ch->v : 0;
        }
        level *= TICK_CLK;
        if (ctx->pokey4000 || ctx->pokey450) {
            level += pokey_tick(ctx);
        }
        ctx->asum += level;
        ctx->an++;
        ctx->aacc += AUDIO_RATE * TICK_CLK;
        if (ctx->aacc >= ctx->clock_hz) {
            ctx->aacc -= ctx->clock_hz;
            int16_t s = (int16_t)(ctx->asum / TICK_CLK * ctx->level_scale / ctx->an);
            ctx->asum = ctx->an = 0;
            ctx->nsamples++;
            if (ctx->ring != NULL) {
                ctx->ring[ctx->ring_pos] = s;
                if (++ctx->ring_pos == ctx->ring_len) {
                    ctx->ring_pos = 0;
                }
            }
        }
    }
}

// ============================================================================
// RIOT (6532): joystick port A, console switches port B, interval timer.
// Timer arithmetic is in MARIA clocks; one RIOT cycle is 4 of them.

static uint8_t riot_rd(ctx_t *ctx, uint32_t a, uint32_t clk) {
    if (!(a & 4)) {
        switch (a & 3) {
            case 0: return (uint8_t)((ctx->swcha_out & ctx->swacnt) | (ctx->swcha_in & ~ctx->swacnt));
            case 1: return ctx->swacnt;
            case 2: return (uint8_t)((ctx->swchb_out & ctx->swbcnt) | (ctx->swchb_in & ~ctx->swbcnt));
            default: return ctx->swbcnt;
        }
    }
    int32_t t = ((int32_t)clk - ctx->tim_base) >> 2;
    if (t < 0) {
        t = 0;
    }
    int32_t under = ((int32_t)ctx->tim_start) << ctx->tim_shift;
    int32_t val;
    int expired = t >= under;
    if (!expired) {
        val = ctx->tim_start - 1 - (t >> ctx->tim_shift);
        if (val < 0) {
            val = 0;
        }
    } else {
        val = (0xFF - (t - under)) & 0xFF;
    }
    if (a & 1) {
        return (expired && !ctx->tim_ack) ? 0x80 : 0x00;   // TIMINT
    }
    if (expired) {
        ctx->tim_ack = 1;
    }
    return (uint8_t)val;
}

static void riot_wr(ctx_t *ctx, uint32_t a, uint8_t v, uint32_t clk) {
    if (!(a & 4)) {
        switch (a & 3) {
            case 0: ctx->swcha_out = v; break;
            case 1: ctx->swacnt = v; break;
            case 2: ctx->swchb_out = v; break;
            default: ctx->swbcnt = v; break;
        }
    } else if (a & 0x10) {
        static const uint8_t shifts[4] = {0, 3, 6, 10};
        ctx->tim_start = v;
        ctx->tim_shift = shifts[a & 3];
        ctx->tim_base = (int32_t)clk + 4;
        ctx->tim_ack = 0;
    }
}

// ============================================================================
// Bus

static uint8_t bus_rd_slow(ctx_t *ctx, uint32_t a, uint32_t clk) {
    if (a < 0x400) {
        uint32_t lo = a & 0xFF;
        if ((a & 0x280) == 0x280) {
            ctx->xclk += 2;
            return riot_rd(ctx, a, clk);
        }
        if (lo < 0x20) {
            ctx->xclk += 2;
            return tia_rd(ctx, lo);
        }
        if (lo < 0x40) {
            return lo == 0x28 ? ctx->vblank : 0;   // MSTAT
        }
        return 0;
    }
    if ((a & 0xFE80) == 0x0480) {
        return ctx->riot_ram[a & 0x7F];
    }
    if (ctx->pokey450 && (a & 0xFFF0) == 0x0450) {
        return pokey_rd(ctx, a, clk);
    }
    if (ctx->pokey4000 && a >= 0x4000 && a < 0x8000) {
        return pokey_rd(ctx, a, clk);
    }
    return 0xFF;
}

static void bus_wr_slow(ctx_t *ctx, uint32_t a, uint8_t v, uint32_t clk) {
    if (a < 0x400) {
        uint32_t lo = a & 0xFF;
        if ((a & 0x280) == 0x280) {
            ctx->xclk += 2;
            riot_wr(ctx, a, v, clk);
        } else if (lo < 0x20) {
            ctx->xclk += 2;
            tia_wr(ctx, lo, v);
        } else if (lo < 0x40) {
            maria_wr(ctx, lo - 0x20, v);
        }
        return;
    }
    if ((a & 0xFE80) == 0x0480) {
        ctx->riot_ram[a & 0x7F] = v;
    } else if (ctx->pokey450 && (a & 0xFFF0) == 0x0450) {
        pokey_wr(ctx, a, v);
    } else if (a >= 0x4000) {
        if (ctx->pokey4000 && a < 0x8000) {
            pokey_wr(ctx, a, v);
        } else {
            cart_bank_wr(ctx, a, v);
        }
    }
}

static inline uint8_t bus_rd(ctx_t *ctx, uint32_t a, uint32_t clk) {
    const uint8_t *p = ctx->rpage[a >> 8];
    if (p) {
        return p[a & 0xFF];
    }
    if (a < 0x200 && (a & 0xC0)) {
        return ctx->ram[0x800 + a];
    }
    return bus_rd_slow(ctx, a, clk);
}

static inline void bus_wr(ctx_t *ctx, uint32_t a, uint8_t v, uint32_t clk) {
    uint8_t *p = ctx->wpage[a >> 8];
    if (p) {
        p[a & 0xFF] = v;
    } else if (a < 0x200 && (a & 0xC0)) {
        ctx->ram[0x800 + a] = v;
    } else {
        bus_wr_slow(ctx, a, v, clk);
    }
}

#ifdef TRACE
static uint16_t trace_pc[64];
static uint32_t trace_n;
#endif

// Run the CPU until clk reaches `until` (MARIA clocks into the frame).
static void cpu_run(ctx_t *ctx, uint32_t until) {
    uint8_t A = ctx->A, X = ctx->X, Y = ctx->Y, S = ctx->S;
    uint16_t PC = ctx->PC;
    unsigned C_ = ctx->C_, V_ = ctx->V_, D_ = ctx->D_, I_ = ctx->I_, N_ = ctx->N_, Z_ = ctx->Z_;
    uint32_t clk = ctx->clk;
#define RD(a) (clk += 4, bus_rd(ctx, (a), clk))
#define WR(a, v) (clk += 4, bus_wr(ctx, (a), (uint8_t)(v), clk))
#define TICK() (clk += 4)
#define JAM() (ctx->jammed = 1)
    while (clk < until && !ctx->jammed) {
        if (ctx->nmi) {
            // DLI: 7 cycle NMI sequence.
            ctx->nmi = 0;
            clk += 8;
            WR(0x100 | S, PC >> 8);
            S--;
            WR(0x100 | S, PC & 0xFF);
            S--;
            WR(0x100 | S, (N_ & 0x80) | (V_ << 6) | 0x20 | (D_ << 3) | (I_ << 2) | (Z_ ? 0 : 2) | C_);
            S--;
            I_ = 1;
            unsigned lo = RD(0xFFFA);
            PC = (uint16_t)(lo | (RD(0xFFFB) << 8));
            continue;
        }
#ifdef TRACE
        trace_pc[trace_n++ & 63] = PC;
#endif
#include "cpu6502.h"
        if (ctx->xclk) {
            clk += ctx->xclk;
            ctx->xclk = 0;
        }
        if (ctx->wsync) {
            // Hold until the end of the line the write landed in.
            ctx->wsync = 0;
            uint32_t end = (ctx->line + 1) * LINE_CLK;
            if (clk > end) {
                end += LINE_CLK;
            }
            clk = end;
        }
    }
#undef RD
#undef WR
#undef TICK
#undef JAM
    ctx->A = A; ctx->X = X; ctx->Y = Y; ctx->S = S; ctx->PC = PC;
    ctx->C_ = C_; ctx->V_ = V_; ctx->D_ = D_; ctx->I_ = I_; ctx->N_ = N_; ctx->Z_ = Z_;
    ctx->clk = clk;
}

// ============================================================================
// Frame loop

static void run_frame(ctx_t *ctx) {
    uint32_t n = ctx->nlines;
    for (uint32_t line = 0; line < n && !ctx->jammed; line++) {
        uint32_t base = line * LINE_CLK;
        ctx->line = line;
        if (line == 16) {
            ctx->vblank = 0;
        } else if (line == n - 5) {
            ctx->vblank = 0x80;
        }
        cpu_run(ctx, base + DMA_AT);
        if (line == 16 && ctx->dmaon) {
            ctx->dll = ctx->dpp;
            load_dll_entry(ctx);
        }
        if (line > 15 && line < n - 5) {
            if (ctx->dmaon) {
                uint32_t mc = maria_dma(ctx);
                uint32_t end = base + LINE_CLK;
                if (ctx->clk < end) {
                    if (mc >= DMA_MAX) {
                        ctx->clk = end;
                    } else {
                        uint32_t c = ctx->clk > base + DMA_AT ? ctx->clk : base + DMA_AT;
                        ctx->clk = c + ((mc + 3) / 4) * 4;
                    }
                }
            }
            display_line(ctx);
        } else {
            blank_row(ctx);
        }
        if (line > 16 && line < n - 5 && ctx->dmaon) {
            if (ctx->offset == 0) {
                ctx->dll += 3;
                load_dll_entry(ctx);
            } else {
                ctx->offset--;
            }
        }
        if (ctx->dli) {
            ctx->dli = 0;
            ctx->nmi = 1;
        }
        cpu_run(ctx, base + LINE_CLK);
        audio_line(ctx);
    }
    // Rebase the clock so it stays small.
    uint32_t k = n * LINE_CLK;
    ctx->clk = ctx->clk > k ? ctx->clk - k : 0;
    ctx->tim_base -= (int32_t)k;
    if (ctx->tim_base < -0x10000000) {
        ctx->tim_base = -0x10000000;
    }
}

// ============================================================================
// Entry points, shared by the host harness and the native module.

static void a78_set_input(ctx_t *ctx, uint32_t m) {
    uint8_t a = 0xFF;
    if (m & IN_RIGHT) a &= ~0x80;
    if (m & IN_LEFT) a &= ~0x40;
    if (m & IN_DOWN) a &= ~0x20;
    if (m & IN_UP) a &= ~0x10;
    ctx->swcha_in = a;
    uint8_t b = 0x37;   // unused inputs read high
    if (m & IN_RESET) b &= ~0x01;
    if (m & IN_SELECT) b &= ~0x02;
    if (!(m & IN_PAUSE)) b |= 0x08;
    if (m & IN_LEFT_A) b |= 0x40;
    if (m & IN_RIGHT_A) b |= 0x80;
    ctx->swchb_in = b;
    ctx->buttons = (uint8_t)(((m & IN_FIRE1) ? 1 : 0) | ((m & IN_FIRE2) ? 2 : 0));
}

static void a78_set_palette(ctx_t *ctx) {
    for (int i = 0; i < 256; i++) {
        uint32_t c = a78_rgb[i];
        uint32_t r = c >> 16, g = (c >> 8) & 0xFF, b = c & 0xFF;
        ctx->pal16[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        ctx->pal8[i] = (uint8_t)(((r >> 5) << 5) | ((g >> 5) << 2) | (b >> 6));
    }
}

// Output geometry, see set_output() at the top. fb may be NULL (no drawing).
// The caller checks that the framebuffer is large enough.
static void a78_set_output(ctx_t *ctx, uint8_t *fb, uint32_t width, uint32_t height, uint32_t bpp, uint32_t top) {
    ctx->fb = fb;
    ctx->fb_width = width;
    ctx->fb_height = height;
    ctx->bpp = bpp;
    ctx->x0 = (width - 320) / 2;
    ctx->top = top;
}

static int a78_init(ctx_t *ctx, const uint8_t *data, uint32_t len) {
    memset(ctx, 0, sizeof(*ctx));
    int e = cart_setup(ctx, data, len);
    if (e) {
        return e;
    }
    for (uint32_t p = 0x18; p < 0x28; p++) {
        ctx->rpage[p] = ctx->wpage[p] = ctx->ram + ((p - 0x18) << 8);
    }
    ctx->nlines = ctx->pal_tv ? 313 : 263;
    ctx->clock_hz = ctx->pal_tv ? CLK_PAL : CLK_NTSC;
    ctx->level_scale = (ctx->pokey4000 || ctx->pokey450) ? 333 : 1000;
    ctx->vblank = 0x80;
    ctx->dmaon = 0;
    a78_set_palette(ctx);
    pokey_init(ctx);
    a78_set_input(ctx, 0);
    a78_set_output(ctx, NULL, 320, 240, 16, 17);
    ctx->tim_start = 0;     // timer starts expired, free running
    ctx->tim_shift = 10;
    ctx->S = 0xFF;
    ctx->I_ = 1;
    ctx->Z_ = 1;
    ctx->PC = (uint16_t)(peek(ctx, 0xFFFC) | (peek(ctx, 0xFFFD) << 8));
    return 0;
}

static uint32_t a78_run_frames(ctx_t *ctx, uint32_t n) {
    uint32_t i = 0;
    while (i < n && !ctx->jammed) {
        run_frame(ctx);
        if (!ctx->jammed) {
            ctx->frames++;
            i++;
        }
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
    return mp_obj_new_int(a78_init(ctx, rom.buf, rom.len));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_init_obj, mod_init);

// info(st) -> (mapper name, rom bytes, pokey, pal)
static mp_obj_t mod_info(mp_obj_t st_in) {
    ctx_t *ctx = get_ctx(st_in);
    const char *name = mapper_name(ctx);
    mp_obj_t t[4] = {
        mp_obj_new_str(name, strlen(name)),
        mp_obj_new_int(ctx->rom_len),
        mp_obj_new_int(ctx->pokey4000 ? 0x4000 : ctx->pokey450 ? 0x450 : 0),
        mp_obj_new_int(ctx->pal_tv),
    };
    return mp_obj_new_tuple(4, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_info_obj, mod_info);

// set_output(st, fb, width, height, bpp, top); fb=None disables drawing.
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
    uint32_t top = mp_obj_get_int(args[5]);
    if ((bpp != 8 && bpp != 16) || width < 320 || (width & 3) || height == 0 ||
        width * height * (bpp / 8) > fb.len) {
        mp_raise_ValueError(MP_ERROR_TEXT("output does not fit"));
    }
    a78_set_output(ctx, fb.buf, width, height, bpp, top);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_set_output_obj, 6, 6, mod_set_output);

// set_audio(st, ring, start): ring is mono int16 (array('h') / bytearray),
// start is the sample to write next. ring=None stops audio output.
static mp_obj_t mod_set_audio(mp_obj_t st_in, mp_obj_t ring_in, mp_obj_t start_in) {
    ctx_t *ctx = get_ctx(st_in);
    if (ring_in == mp_const_none) {
        ctx->ring = NULL;
        return mp_const_none;
    }
    mp_buffer_info_t ring;
    mp_get_buffer_raise(ring_in, &ring, MP_BUFFER_WRITE);
    uint32_t len = ring.len / 2;
    uint32_t start = mp_obj_get_int(start_in);
    if (len == 0 || start >= len) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad audio ring"));
    }
    ctx->ring = ring.buf;
    ctx->ring_len = len;
    ctx->ring_pos = start;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_set_audio_obj, mod_set_audio);

static mp_obj_t mod_set_input(mp_obj_t st_in, mp_obj_t mask_in) {
    a78_set_input(get_ctx(st_in), mp_obj_get_int(mask_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_set_input_obj, mod_set_input);

static mp_obj_t mod_run_frames(mp_obj_t st_in, mp_obj_t n_in) {
    return mp_obj_new_int(a78_run_frames(get_ctx(st_in), mp_obj_get_int(n_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_run_frames_obj, mod_run_frames);

static mp_obj_t mod_samples(mp_obj_t st_in) {
    return mp_obj_new_int_from_uint(get_ctx(st_in)->nsamples);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_samples_obj, mod_samples);

// frame_crc(st, enable=None): the CRC is only computed while enabled.
static mp_obj_t mod_frame_crc(size_t n_args, const mp_obj_t *args) {
    ctx_t *ctx = get_ctx(args[0]);
    if (n_args > 1) {
        ctx->crc_on = mp_obj_is_true(args[1]);
    }
    return mp_obj_new_int_from_uint(ctx->frame_crc);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_frame_crc_obj, 1, 2, mod_frame_crc);

// error(st) -> PC of a JAM opcode, or None.
static mp_obj_t mod_error(mp_obj_t st_in) {
    ctx_t *ctx = get_ctx(st_in);
    return ctx->jammed ? mp_obj_new_int(ctx->PC) : mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_error_obj, mod_error);

mp_obj_t mpy_init(mp_obj_fun_bc_t *self, size_t n_args, size_t n_kw, mp_obj_t *args) {
    MP_DYNRUNTIME_INIT_ENTRY
    mp_store_global(MP_QSTR_STATE_SIZE, MP_OBJ_NEW_SMALL_INT(sizeof(ctx_t)));
    mp_store_global(MP_QSTR_AUDIO_RATE, MP_OBJ_NEW_SMALL_INT(AUDIO_RATE));
    mp_store_global(MP_QSTR_INPUT_UP, MP_OBJ_NEW_SMALL_INT(IN_UP));
    mp_store_global(MP_QSTR_INPUT_DOWN, MP_OBJ_NEW_SMALL_INT(IN_DOWN));
    mp_store_global(MP_QSTR_INPUT_LEFT, MP_OBJ_NEW_SMALL_INT(IN_LEFT));
    mp_store_global(MP_QSTR_INPUT_RIGHT, MP_OBJ_NEW_SMALL_INT(IN_RIGHT));
    mp_store_global(MP_QSTR_INPUT_FIRE1, MP_OBJ_NEW_SMALL_INT(IN_FIRE1));
    mp_store_global(MP_QSTR_INPUT_FIRE2, MP_OBJ_NEW_SMALL_INT(IN_FIRE2));
    mp_store_global(MP_QSTR_INPUT_RESET, MP_OBJ_NEW_SMALL_INT(IN_RESET));
    mp_store_global(MP_QSTR_INPUT_SELECT, MP_OBJ_NEW_SMALL_INT(IN_SELECT));
    mp_store_global(MP_QSTR_INPUT_PAUSE, MP_OBJ_NEW_SMALL_INT(IN_PAUSE));
    mp_store_global(MP_QSTR_INPUT_LEFT_A, MP_OBJ_NEW_SMALL_INT(IN_LEFT_A));
    mp_store_global(MP_QSTR_INPUT_RIGHT_A, MP_OBJ_NEW_SMALL_INT(IN_RIGHT_A));
    mp_store_global(MP_QSTR_init, MP_OBJ_FROM_PTR(&mod_init_obj));
    mp_store_global(MP_QSTR_info, MP_OBJ_FROM_PTR(&mod_info_obj));
    mp_store_global(MP_QSTR_set_output, MP_OBJ_FROM_PTR(&mod_set_output_obj));
    mp_store_global(MP_QSTR_set_input, MP_OBJ_FROM_PTR(&mod_set_input_obj));
    mp_store_global(MP_QSTR_set_audio, MP_OBJ_FROM_PTR(&mod_set_audio_obj));
    mp_store_global(MP_QSTR_run_frames, MP_OBJ_FROM_PTR(&mod_run_frames_obj));
    mp_store_global(MP_QSTR_samples, MP_OBJ_FROM_PTR(&mod_samples_obj));
    mp_store_global(MP_QSTR_frame_crc, MP_OBJ_FROM_PTR(&mod_frame_crc_obj));
    mp_store_global(MP_QSTR_error, MP_OBJ_FROM_PTR(&mod_error_obj));
    MP_DYNRUNTIME_INIT_EXIT
}

#endif
