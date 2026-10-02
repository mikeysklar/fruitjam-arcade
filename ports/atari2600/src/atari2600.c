// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Native module `atari2600`: an Atari 2600 (6507 + TIA + RIOT) written for
// this project, as a CircuitPython native .mpy.
//
// Every buffer lives in a Python-owned bytearray and is passed in, so the GC
// sees all of it and nothing has to be allocated from native code:
//
//   st = bytearray(atari2600.STATE_SIZE)        # whole machine, keep alive
//   atari2600.init(st, rom)                     # rom: bytes, keep alive; -> 0 ok,
//                                               # 1 unsupported size, 2 ARM cart (DPC+/CDF)
//   atari2600.mapper(st)                        # -> "4K", "F8", "F6SC", ...
//   atari2600.set_output(st, fb, width, height, bpp, top)   # optional
//   atari2600.set_input(st, mask)               # INPUT_* bits below, 1 = pressed/on
//   atari2600.run_frames(st, n)                 # -> frames run, stops on a JAM opcode
//   atari2600.set_audio(st, ring, start)        # optional, see below
//   atari2600.samples(st)                       # audio samples made since init
//   atari2600.frame_crc(st)                     # CRC-32 of the last frame (if enabled)
//
// Video: each 160 pixel TIA line is drawn 2x wide into a framebuffer row, so
// width must be at least 320. Scanline `top` (counted from the end of VSYNC)
// lands on framebuffer row 0; `height` rows are drawn. bpp 16 is RGB565,
// bpp 8 is RGB332.
//
// Sound: two TIA channels mixed to mono int16 at AUDIO_RATE, resampled from
// the 31.4 kHz TIA audio clock. With a ring set, samples are appended as they
// are made, wrapping. Make the ring the buffer of a looping, double buffered
// audiocore.RawSample and start writing one half ahead of playback.
//
// Timing model: the CPU counts every bus cycle. TIA state is only changed by
// CPU writes, so the TIA renders lazily: before each TIA access it draws all
// color clocks up to the current one with the registers as they were. A frame
// ends when VSYNC is switched on (or after 400 lines without one).

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
#define TIA_AUDIO_HZ2 62799   // 2 x 31399.5 Hz TIA audio clock (3579545 / 114)
#define LINE_CLK 228
#define HBLANK 68
#define MAX_LINES 400

// set_input() bits
#define IN_UP 0x001
#define IN_DOWN 0x002
#define IN_LEFT 0x004
#define IN_RIGHT 0x008
#define IN_FIRE 0x010
#define IN_RESET 0x020
#define IN_SELECT 0x040
#define IN_BW 0x080         // color/B&W switch set to B&W
#define IN_LEFT_A 0x100     // left difficulty set to A (pro)
#define IN_RIGHT_A 0x200    // right difficulty set to A (pro)

// Object bits in the per-pixel mask.
#define O_PF 0x01
#define O_BL 0x02
#define O_M0 0x04
#define O_M1 0x08
#define O_P0 0x10
#define O_P1 0x20

enum { BK_2K, BK_4K, BK_F8, BK_F6, BK_F4, BK_FE, BK_E0, BK_3F };

typedef struct {
    uint8_t c, f, v;        // AUDC, AUDF, AUDV
    uint8_t div;            // AUDF divider count
    uint8_t out;            // current output bit
    uint8_t d31, d3;        // div 31 and div 3 counters
    uint8_t p4, p5;         // 4 and 5 bit polynomial counters
    uint16_t p9;            // 9 bit polynomial counter
} chan_t;

typedef struct {
    // ---- cartridge ----
    const uint8_t *rom;
    uint32_t rom_len;
    uint32_t bank_type;
    uint32_t superchip;
    uint32_t nbanks;
    uint32_t hot_lo, hot_hi;       // F8/F6/F4 hotspot range (offsets in 4K)
    uint32_t fe_armed;
    uint32_t fast, fast_lo;        // bus fast path on; first ROM offset it covers
    const uint8_t *slice[4];       // 1K windows at 0x1000, 0x1400, 0x1800, 0x1C00
    uint8_t sc_ram[128];

    // ---- CPU ----
    uint8_t A, X, Y, S;
    uint16_t PC;
    uint8_t jammed;
    uint8_t stop;
    uint32_t C_, V_, D_, I_, N_, Z_;
    uint32_t cyc;                  // CPU cycles since the last rebase

    // ---- RIOT ----
    uint8_t ram[128];
    uint8_t swcha_in, swchb_in, swacnt, swbcnt, swcha_out, swchb_out;
    uint8_t tim_start, tim_shift, tim_ack, fire;
    int32_t tim_base;              // cycle the timer was written (end of write)

    // ---- TIA ----
    uint32_t tclk;                 // color clock rendered up to
    uint32_t line_clk;             // color clock at the start of this line
    uint32_t line;                 // scanline counted from the end of VSYNC
    uint32_t frame_lines;          // lines since the frame started
    uint32_t vis_start, vis_end;   // lines where VBLANK last went off / on
    uint8_t vsync, vblank, ctrlpf, hmove_blank;
    uint8_t nusiz[2], refp[2], vdelp[2], resmp[2], enam[2];
    uint8_t grp_new[2], grp_old[2];
    uint8_t enabl_new, enabl_old, vdelbl;
    uint8_t pf0, pf1, pf2;
    int8_t hm[5];                  // P0 P1 M0 M1 BL
    uint8_t pos[5];                // P0 P1 M0 M1 BL, pixel 0..159
    uint8_t dirty;                 // objects whose mask bits need redrawing
    uint8_t col[4];                // palette index: BK, PF, P0, P1
    uint16_t coll;                 // collision latches, see tia_read
    uint8_t pfl[40];               // playfield bit per 4 pixel block
    uint8_t prio[2][64];           // object mask -> col[] index, left/right half
    uint16_t colltab[64];
    uint8_t objm[160] __attribute__((aligned(4)));
    uint32_t col32[4];             // col[] as output pixels (pal32 entries)
    uint8_t *row;                  // output row for the current line
    uint32_t scratch[160];         // row for lines outside the framebuffer

    // ---- audio ----
    chan_t ch[2];
    uint32_t aacc, asum, an;
    uint32_t nsamples;
    int16_t *ring;
    uint32_t ring_len, ring_pos;

    // ---- output ----
    uint8_t *fb;
    uint32_t fb_width, fb_height, bpp, x0, top;
    uint32_t crc_on, crc, frame_crc;
    uint32_t pal32[128];           // RGB565 pixel doubled, or RGB332 doubled
    uint32_t frames;
} ctx_t;

// ============================================================================
// Cartridge

static void set_4k(ctx_t *ctx, uint32_t b) {
    for (int i = 0; i < 4; i++) {
        ctx->slice[i] = ctx->rom + b * 4096 + i * 1024;
    }
}

static void cart_hot(ctx_t *ctx, uint32_t off) {
    switch (ctx->bank_type) {
        case BK_F8:
        case BK_F6:
        case BK_F4:
            if (off >= ctx->hot_lo && off <= ctx->hot_hi) {
                set_4k(ctx, off - ctx->hot_lo);
            }
            break;
        case BK_E0:
            if (off >= 0xFE0 && off <= 0xFF7) {
                ctx->slice[(off - 0xFE0) >> 3] = ctx->rom + (off & 7) * 1024;
            }
            break;
    }
}

static inline uint8_t cart_rd(ctx_t *ctx, uint32_t a) {
    uint32_t off = a & 0xFFF;
    if (off >= 0xFE0) {
        cart_hot(ctx, off);
    }
    if (ctx->superchip && off < 0x100) {
        return off >= 0x80 ? ctx->sc_ram[off & 0x7F] : 0xFF;
    }
    return ctx->slice[off >> 10][off & 0x3FF];
}

static inline void cart_wr(ctx_t *ctx, uint32_t a, uint8_t v) {
    uint32_t off = a & 0xFFF;
    if (off >= 0xFE0) {
        cart_hot(ctx, off);
    }
    if (ctx->superchip && off < 0x80) {
        ctx->sc_ram[off] = v;
    }
}

static uint32_t count_sig(const uint8_t *rom, uint32_t len, const uint8_t *sig, uint32_t n) {
    uint32_t hits = 0;
    for (uint32_t i = 0; i + n <= len; i++) {
        uint32_t k = 0;
        while (k < n && rom[i + k] == sig[k]) {
            k++;
        }
        hits += k == n;
    }
    return hits;
}

// Superchip carts leave the first 256 bytes of every 4K bank (the RAM ports)
// filled with one value.
static uint32_t is_superchip(const uint8_t *rom, uint32_t len) {
    for (uint32_t b = 0; b < len; b += 4096) {
        for (uint32_t i = 1; i < 256; i++) {
            if (rom[b + i] != rom[b]) {
                return 0;
            }
        }
    }
    return 1;
}

// Count absolute loads/stores/BITs to $xFF4-$xFFB (F4/F6/F8 hotspots).
static uint32_t f_hotspot_refs(const uint8_t *rom, uint32_t len) {
    static const uint8_t ops[] = {0xAD, 0x8D, 0x2C, 0xCD, 0xAE, 0xAC, 0x8E, 0x8C, 0x0C, 0xBD, 0x9D, 0xB9, 0x99};
    uint32_t hits = 0;
    for (uint32_t i = 0; i + 2 < len; i++) {
        if (rom[i + 1] >= 0xF4 && rom[i + 1] <= 0xFB && (rom[i + 2] & 0x1F) == 0x1F) {
            for (unsigned k = 0; k < sizeof ops; k++) {
                hits += rom[i] == ops[k];
            }
        }
    }
    return hits;
}

// Pick the bankswitch scheme. Returns 0, or 1 for an unsupported size, 2 for
// an ARM (DPC+/CDF) cartridge.
static int cart_detect(ctx_t *ctx) {
    const uint8_t *rom = ctx->rom;
    uint32_t len = ctx->rom_len;
    // Byte patterns as used by common 2600 cart detectors: E0 hotspot
    // accesses, Tigervision STA $3F, Activision JSR/RTS sequences.
    static const uint8_t e0[][3] = {
        {0x8D, 0xE0, 0x1F}, {0x8D, 0xE0, 0x5F}, {0x8D, 0xE9, 0xFF}, {0x0C, 0xE0, 0x1F},
        {0xAD, 0xE0, 0x1F}, {0xAD, 0xE9, 0xFF}, {0xAD, 0xED, 0xFF}, {0xAD, 0xF3, 0xBF},
    };
    static const uint8_t fe[][5] = {
        {0x20, 0x00, 0xD0, 0xC6, 0xC5}, {0x20, 0xC3, 0xF8, 0xA5, 0x82},
        {0xD0, 0xFB, 0x20, 0x73, 0xFE}, {0x20, 0x00, 0xF0, 0x84, 0xD6},
    };
    static const uint8_t t3f[2] = {0x85, 0x3F};
    static const uint8_t dpcp[4] = {'D', 'P', 'C', '+'};
    static const uint8_t cdf[3] = {'C', 'D', 'F'};
    ctx->superchip = 0;
    if (len == 0 || len > 512 * 1024) {
        return 1;
    }
    // Harmony/Melody carts run ARM code on the cartridge: not emulated.
    if (count_sig(rom, len, dpcp, 4) || count_sig(rom, len, cdf, 3) >= 3) {
        return 2;
    }
    if (len <= 4096) {
        if (len & (len - 1)) {
            return 1;           // not a power of two
        }
        ctx->bank_type = len == 4096 ? BK_4K : BK_2K;
        for (int i = 0; i < 4; i++) {
            ctx->slice[i] = rom + ((i * 1024) & (len - 1));
        }
        if (len < 1024) {
            return 1;
        }
        return 0;
    }
    uint32_t n3f = count_sig(rom, len, t3f, 2);
    if (len == 8192) {
        for (unsigned i = 0; i < sizeof e0 / sizeof e0[0]; i++) {
            if (count_sig(rom, len, e0[i], 3)) {
                ctx->bank_type = BK_E0;
                ctx->slice[0] = rom + 4 * 1024;
                ctx->slice[1] = rom + 5 * 1024;
                ctx->slice[2] = rom + 6 * 1024;
                ctx->slice[3] = rom + 7 * 1024;
                return 0;
            }
        }
    }
    // "85 3F" can turn up by chance in data; an absolute access to an F8/F6/F4
    // hotspot says it is one of those instead.
    if (n3f >= 2 && (len & 2047) == 0 && !(len <= 32768 && f_hotspot_refs(rom, len))) {
        ctx->bank_type = BK_3F;
        ctx->nbanks = len / 2048;
        ctx->slice[0] = rom;
        ctx->slice[1] = rom + 1024;
        ctx->slice[2] = rom + len - 2048;
        ctx->slice[3] = rom + len - 1024;
        return 0;
    }
    if (len == 8192) {
        for (unsigned i = 0; i < sizeof fe / sizeof fe[0]; i++) {
            if (count_sig(rom, len, fe[i], 5)) {
                ctx->bank_type = BK_FE;
                set_4k(ctx, 0);
                return 0;
            }
        }
    }
    if (len == 8192 || len == 16384 || len == 32768) {
        ctx->nbanks = len / 4096;
        ctx->bank_type = len == 8192 ? BK_F8 : len == 16384 ? BK_F6 : BK_F4;
        ctx->hot_lo = len == 8192 ? 0xFF8 : len == 16384 ? 0xFF6 : 0xFF4;
        ctx->hot_hi = 0xFF9 + (len == 32768 ? 2 : 0);
        ctx->superchip = is_superchip(rom, len);
        set_4k(ctx, ctx->nbanks - 1);   // start in the last bank
        return 0;
    }
    return 1;
}

// Scheme name, "SC" appended for Superchip RAM. Returns the length; buf needs 5 bytes.
static size_t mapper_name(ctx_t *ctx, char *buf) {
    static const char names[8][2] = {
        {'2', 'K'}, {'4', 'K'}, {'F', '8'}, {'F', '6'}, {'F', '4'}, {'F', 'E'}, {'E', '0'}, {'3', 'F'},
    };
    size_t n = 0;
    buf[n++] = names[ctx->bank_type][0];
    buf[n++] = names[ctx->bank_type][1];
    if (ctx->superchip) {
        buf[n++] = 'S';
        buf[n++] = 'C';
    }
    buf[n] = 0;
    return n;
}

// ============================================================================
// TIA video

static void pf_update(ctx_t *ctx) {
    uint32_t bits = ((ctx->pf0 >> 4) & 0x0F);         // 20 bits, bit 0 leftmost
    for (int i = 0; i < 8; i++) {
        bits |= ((ctx->pf1 >> (7 - i)) & 1) << (4 + i);
    }
    bits |= (uint32_t)ctx->pf2 << 12;
    for (int i = 0; i < 20; i++) {
        ctx->pfl[i] = (bits >> i) & 1;
        ctx->pfl[20 + i] = (ctx->ctrlpf & 1) ? (bits >> (19 - i)) & 1 : (bits >> i) & 1;
    }
}

static void prio_update(ctx_t *ctx) {
    uint32_t pfp = ctx->ctrlpf & 4, score = (ctx->ctrlpf & 2) && !pfp;
    for (int half = 0; half < 2; half++) {
        uint8_t pfc = score ? 2 + half : 1;
        for (int m = 0; m < 64; m++) {
            uint8_t c;
            uint32_t p0 = m & (O_P0 | O_M0), p1 = m & (O_P1 | O_M1);
            if (pfp && (m & O_PF)) {
                c = 1;
            } else if (pfp && (m & O_BL)) {
                c = 1;
            } else if (p0) {
                c = 2;
            } else if (p1) {
                c = 3;
            } else if (m & O_PF) {
                c = pfc;
            } else if (m & O_BL) {
                c = 1;
            } else {
                c = 0;
            }
            ctx->prio[half][m] = c;
        }
    }
}

// Collision latch bits: register r (0..7) bit 7 is bit 2r, bit 6 is bit 2r+1.
static void colltab_init(ctx_t *ctx) {
    static const uint8_t pairs[15][3] = {
        {0, O_M0, O_P1}, {1, O_M0, O_P0}, {2, O_M1, O_P0}, {3, O_M1, O_P1},
        {4, O_P0, O_PF}, {5, O_P0, O_BL}, {6, O_P1, O_PF}, {7, O_P1, O_BL},
        {8, O_M0, O_PF}, {9, O_M0, O_BL}, {10, O_M1, O_PF}, {11, O_M1, O_BL},
        {12, O_BL, O_PF}, {14, O_P0, O_P1}, {15, O_M0, O_M1},
    };
    for (int m = 0; m < 64; m++) {
        uint16_t c = 0;
        for (int i = 0; i < 15; i++) {
            if ((m & pairs[i][1]) && (m & pairs[i][2])) {
                c |= 1 << pairs[i][0];
            }
        }
        ctx->colltab[m] = c;
    }
}

// NUSIZ copy offsets, 0 terminated after the first entry.
static const uint8_t copies[8][4] = {
    {0, 0, 0, 0}, {0, 16, 0, 0}, {0, 32, 0, 0}, {0, 16, 32, 0},
    {0, 64, 0, 0}, {0, 0, 0, 0}, {0, 32, 64, 0}, {0, 0, 0, 0},
};

static void mask_clear(ctx_t *ctx, uint8_t bit) {
    uint32_t *m = (uint32_t *)ctx->objm;
    uint32_t k = ~(bit * 0x01010101u);
    for (int i = 0; i < 40; i++) {
        m[i] &= k;
    }
}

static void mask_span(ctx_t *ctx, uint32_t x, uint32_t w, uint8_t bit) {
    for (uint32_t i = 0; i < w; i++) {
        uint32_t p = x + i;
        if (p >= 160) {
            p -= 160;
        }
        ctx->objm[p] |= bit;
    }
}

static void draw_player(ctx_t *ctx, int i) {
    uint8_t bit = i ? O_P1 : O_P0;
    mask_clear(ctx, bit);
    uint8_t g = ctx->vdelp[i] ? ctx->grp_old[i] : ctx->grp_new[i];
    if (g == 0) {
        return;
    }
    if (ctx->refp[i] & 8) {
        g = (uint8_t)(((g * 0x0802u & 0x22110u) | (g * 0x8020u & 0x88440u)) * 0x10101u >> 16);
    }
    uint32_t mode = ctx->nusiz[i] & 7;
    uint32_t sh = mode == 5 ? 1 : mode == 7 ? 2 : 0;
    uint32_t start = ctx->pos[i] + (sh ? 1 : 0);
    for (int c = 0; c < 4; c++) {
        if (c && copies[mode][c] == 0) {
            break;
        }
        uint32_t x = start + copies[mode][c];
        for (uint32_t k = 0; k < (8u << sh); k++) {
            if (g & (0x80 >> (k >> sh))) {
                uint32_t p = x + k;
                while (p >= 160) {
                    p -= 160;
                }
                ctx->objm[p] |= bit;
            }
        }
    }
}

static void draw_missile(ctx_t *ctx, int i) {
    uint8_t bit = i ? O_M1 : O_M0;
    mask_clear(ctx, bit);
    if (!(ctx->enam[i] & 2) || (ctx->resmp[i] & 2)) {
        return;
    }
    uint32_t mode = ctx->nusiz[i] & 7;
    uint32_t w = 1u << ((ctx->nusiz[i] >> 4) & 3);
    for (int c = 0; c < 4; c++) {
        if (c && copies[mode][c] == 0) {
            break;
        }
        uint32_t x = ctx->pos[2 + i] + copies[mode][c];
        if (x >= 160) {
            x -= 160;
        }
        mask_span(ctx, x, w, bit);
    }
}

static void draw_ball(ctx_t *ctx) {
    mask_clear(ctx, O_BL);
    uint8_t en = ctx->vdelbl ? ctx->enabl_old : ctx->enabl_new;
    if (!(en & 2)) {
        return;
    }
    mask_span(ctx, ctx->pos[4], 1u << ((ctx->ctrlpf >> 4) & 3), O_BL);
}

static void redraw_objects(ctx_t *ctx) {
    uint8_t d = ctx->dirty;
    ctx->dirty = 0;
    if (d & O_P0) {
        draw_player(ctx, 0);
    }
    if (d & O_P1) {
        draw_player(ctx, 1);
    }
    if (d & O_M0) {
        draw_missile(ctx, 0);
    }
    if (d & O_M1) {
        draw_missile(ctx, 1);
    }
    if (d & O_BL) {
        draw_ball(ctx);
    }
}

// Draw pixels [x, x1) of the current line straight into the output row, one
// framebuffer pixel pair per TIA pixel (T is uint32_t for RGB565, uint16_t
// for RGB332). Four pixel groups with no objects take a fast path.
#define RENDER_FN(name, T) \
    static void name(ctx_t *ctx, T *out, uint32_t x, uint32_t x1) { \
        const uint8_t *objm = ctx->objm; \
        const uint8_t *pfl = ctx->pfl; \
        const uint32_t *cv = ctx->col32; \
        const uint16_t *ct = ctx->colltab; \
        uint32_t coll = ctx->coll; \
        while (x < x1) { \
            const uint8_t *prio = ctx->prio[x >= 80]; \
            if (!(x & 3) && x + 4 <= x1 && *(const uint32_t *)(objm + x) == 0) { \
                T c = (T)cv[prio[pfl[x >> 2]]]; \
                out[x] = c; \
                out[x + 1] = c; \
                out[x + 2] = c; \
                out[x + 3] = c; \
                x += 4; \
                continue; \
            } \
            uint32_t m = objm[x] | pfl[x >> 2]; \
            coll |= ct[m]; \
            out[x] = (T)cv[prio[m]]; \
            x++; \
        } \
        ctx->coll = (uint16_t)coll; \
    }
RENDER_FN(render16, uint32_t)
RENDER_FN(render8, uint16_t)

static void render(ctx_t *ctx, uint32_t x0, uint32_t x1) {
    if (ctx->vblank & 2) {
        memset(ctx->row + x0 * (ctx->bpp / 4), 0, (x1 - x0) * (ctx->bpp / 4));
        return;
    }
    if (ctx->dirty) {
        redraw_objects(ctx);
    }
    if (ctx->bpp == 16) {
        render16(ctx, (uint32_t *)ctx->row, x0, x1);
    } else {
        render8(ctx, (uint16_t *)ctx->row, x0, x1);
    }
    if (ctx->hmove_blank && x0 < 8) {
        uint32_t e = x1 < 8 ? x1 : 8;
        memset(ctx->row + x0 * (ctx->bpp / 4), 0, (e - x0) * (ctx->bpp / 4));
    }
}

// Point `row` at the framebuffer row for the current line, or at scratch.
static void set_row(ctx_t *ctx) {
    uint32_t y = ctx->line - ctx->top;
    if (ctx->fb != NULL && y < ctx->fb_height) {
        ctx->row = ctx->fb + (y * ctx->fb_width + ctx->x0) * (ctx->bpp / 8);
    } else {
        ctx->row = (uint8_t *)ctx->scratch;
    }
}

static void col_update(ctx_t *ctx) {
    for (int i = 0; i < 4; i++) {
        ctx->col32[i] = ctx->pal32[ctx->col[i]];
    }
}

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n) {
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320 & -(c & 1));
        }
    }
    return c;
}

static void audio_line(ctx_t *ctx);

static void end_frame(ctx_t *ctx) {
    ctx->stop = 1;
    ctx->frame_lines = 0;
    ctx->frame_crc = ~ctx->crc;
    ctx->crc = 0xFFFFFFFF;
}

static void end_line(ctx_t *ctx) {
    // Host test only: CRC of the output rows inside the window.
    if (ctx->crc_on && ctx->line - ctx->top < ctx->fb_height) {
        ctx->crc = crc32_update(ctx->crc, ctx->row, 160 * (ctx->bpp / 4));
    }
    ctx->hmove_blank = 0;
    audio_line(ctx);
    if (ctx->line < 1000) {
        ctx->line++;
    }
    set_row(ctx);
    ctx->line_clk += LINE_CLK;
    if (++ctx->frame_lines >= MAX_LINES) {
        end_frame(ctx);
    }
}

// Render every color clock before `target`.
static void tia_sync(ctx_t *ctx, uint32_t target) {
    while (ctx->tclk < target) {
        uint32_t x = ctx->tclk - ctx->line_clk;
        uint32_t end = target - ctx->line_clk;
        if (end > LINE_CLK) {
            end = LINE_CLK;
        }
        if (end > HBLANK) {
            render(ctx, (x < HBLANK ? HBLANK : x) - HBLANK, end - HBLANK);
        }
        ctx->tclk = ctx->line_clk + end;
        if (end == LINE_CLK) {
            end_line(ctx);
        }
    }
}

static uint8_t tia_rd(ctx_t *ctx, uint32_t a, uint32_t c) {
    uint32_t r = a & 0x0F;
    if (r < 8) {
        tia_sync(ctx, (c + 1) * 3);
        uint32_t k = ctx->coll >> (2 * r);
        return (uint8_t)(((k & 1) << 7) | ((k & 2) << 5));
    }
    if (r == 0x0C) {
        return ctx->fire ? 0x00 : 0x80;
    }
    if (r == 0x0D) {
        return 0x80;
    }
    return 0x00;            // paddles: not emulated
}

static uint32_t tia_wr(ctx_t *ctx, uint32_t a, uint8_t v, uint32_t c) {
    uint32_t end = c + 1;
    tia_sync(ctx, end * 3);
    int32_t x = (int32_t)(end * 3 - ctx->line_clk) - HBLANK;   // beam pixel
    switch (a & 0x3F) {
        case 0x00:
            if ((v & 2) && !(ctx->vsync & 2)) {
                end_frame(ctx);
            } else if (!(v & 2) && (ctx->vsync & 2)) {
                ctx->line = 0;
                set_row(ctx);
            }
            ctx->vsync = v;
            break;
        case 0x01:
            if ((ctx->vblank & 2) && !(v & 2)) {
                ctx->vis_start = ctx->line;
            } else if (!(ctx->vblank & 2) && (v & 2)) {
                ctx->vis_end = ctx->line;
            }
            ctx->vblank = v;
            break;
        case 0x02: {
            // WSYNC: halt until the next line starts.
            uint32_t ls = ctx->line_clk / 3;
            if (end > ls) {
                end = ls + LINE_CLK / 3;
            }
        } break;
        case 0x04: ctx->nusiz[0] = v; ctx->dirty |= O_P0 | O_M0; break;
        case 0x05: ctx->nusiz[1] = v; ctx->dirty |= O_P1 | O_M1; break;
        case 0x06: ctx->col[2] = v >> 1; ctx->col32[2] = ctx->pal32[v >> 1]; break;
        case 0x07: ctx->col[3] = v >> 1; ctx->col32[3] = ctx->pal32[v >> 1]; break;
        case 0x08: ctx->col[1] = v >> 1; ctx->col32[1] = ctx->pal32[v >> 1]; break;
        case 0x09: ctx->col[0] = v >> 1; ctx->col32[0] = ctx->pal32[v >> 1]; break;
        case 0x0A:
            ctx->ctrlpf = v;
            pf_update(ctx);
            prio_update(ctx);
            ctx->dirty |= O_BL;
            break;
        case 0x0B: ctx->refp[0] = v; ctx->dirty |= O_P0; break;
        case 0x0C: ctx->refp[1] = v; ctx->dirty |= O_P1; break;
        case 0x0D: ctx->pf0 = v; pf_update(ctx); break;
        case 0x0E: ctx->pf1 = v; pf_update(ctx); break;
        case 0x0F: ctx->pf2 = v; pf_update(ctx); break;
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: {
            uint32_t o = (a & 0x3F) - 0x10;
            uint32_t p;
            if (x < 0) {
                p = o < 2 ? 3 : 2;
            } else {
                p = x + (o < 2 ? 5 : 4);
                if (p >= 160) {
                    p -= 160;
                }
            }
            ctx->pos[o] = (uint8_t)p;
            static const uint8_t bits[5] = {O_P0, O_P1, O_M0, O_M1, O_BL};
            ctx->dirty |= bits[o];
        } break;
        case 0x15: ctx->ch[0].c = v & 0x0F; break;
        case 0x16: ctx->ch[1].c = v & 0x0F; break;
        case 0x17: ctx->ch[0].f = v & 0x1F; break;
        case 0x18: ctx->ch[1].f = v & 0x1F; break;
        case 0x19: ctx->ch[0].v = v & 0x0F; break;
        case 0x1A: ctx->ch[1].v = v & 0x0F; break;
        case 0x1B:
            ctx->grp_new[0] = v;
            ctx->grp_old[1] = ctx->grp_new[1];
            ctx->dirty |= O_P0 | O_P1;
            break;
        case 0x1C:
            ctx->grp_new[1] = v;
            ctx->grp_old[0] = ctx->grp_new[0];
            ctx->enabl_old = ctx->enabl_new;
            ctx->dirty |= O_P0 | O_P1 | O_BL;
            break;
        case 0x1D: ctx->enam[0] = v; ctx->dirty |= O_M0; break;
        case 0x1E: ctx->enam[1] = v; ctx->dirty |= O_M1; break;
        case 0x1F: ctx->enabl_new = v; ctx->dirty |= O_BL; break;
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24:
            ctx->hm[(a & 0x3F) - 0x20] = (int8_t)v >> 4;
            break;
        case 0x25: ctx->vdelp[0] = v & 1; ctx->dirty |= O_P0; break;
        case 0x26: ctx->vdelp[1] = v & 1; ctx->dirty |= O_P1; break;
        case 0x27: ctx->vdelbl = v & 1; ctx->dirty |= O_BL; break;
        case 0x28: case 0x29: {
            int i = (a & 0x3F) - 0x28;
            if ((ctx->resmp[i] & 2) && !(v & 2)) {
                uint32_t mode = ctx->nusiz[i] & 7;
                uint32_t p = ctx->pos[i] + (mode == 5 ? 6 : mode == 7 ? 10 : 3);
                ctx->pos[2 + i] = (uint8_t)(p >= 160 ? p - 160 : p);
            }
            ctx->resmp[i] = v;
            ctx->dirty |= i ? O_M1 : O_M0;
        } break;
        case 0x2A:
            for (int i = 0; i < 5; i++) {
                int32_t p = (int32_t)ctx->pos[i] - ctx->hm[i];
                if (p < 0) {
                    p += 160;
                } else if (p >= 160) {
                    p -= 160;
                }
                ctx->pos[i] = (uint8_t)p;
            }
            if (x < 0) {
                ctx->hmove_blank = 1;
            }
            ctx->dirty = O_P0 | O_P1 | O_M0 | O_M1 | O_BL;
            break;
        case 0x2B:
            for (int i = 0; i < 5; i++) {
                ctx->hm[i] = 0;
            }
            break;
        case 0x2C: ctx->coll = 0; break;
    }
    return end;
}

// ============================================================================
// TIA audio

static void chan_clock(chan_t *ch) {
    // 4, 5 and 9 bit LFSRs, advanced on demand.
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

// Two TIA audio clocks per scanline.
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
        ctx->asum += level;
        ctx->an++;
        ctx->aacc += AUDIO_RATE * 2;
        if (ctx->aacc >= TIA_AUDIO_HZ2) {
            ctx->aacc -= TIA_AUDIO_HZ2;
            int16_t s = (int16_t)(ctx->asum * 1000 / ctx->an);
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
// RIOT

static uint8_t riot_rd(ctx_t *ctx, uint32_t a, uint32_t c) {
    if (!(a & 4)) {
        switch (a & 3) {
            case 0: return (uint8_t)((ctx->swcha_out & ctx->swacnt) | (ctx->swcha_in & ~ctx->swacnt));
            case 1: return ctx->swacnt;
            case 2: return (uint8_t)((ctx->swchb_out & ctx->swbcnt) | (ctx->swchb_in & ~ctx->swbcnt));
            default: return ctx->swbcnt;
        }
    }
    // Timer: counts start-1 down once per interval from the end of the write,
    // then after passing zero wraps to 0xFF and counts once per cycle.
    int32_t t = (int32_t)c - ctx->tim_base;
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

static void riot_wr(ctx_t *ctx, uint32_t a, uint8_t v, uint32_t c) {
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
        ctx->tim_base = (int32_t)c + 1;
        ctx->tim_ack = 0;
    }
}

// ============================================================================
// Bus

static inline void fe_watch(ctx_t *ctx, uint32_t a, uint8_t v) {
    if (ctx->fe_armed) {
        ctx->fe_armed = 0;
        set_4k(ctx, (v & 0x20) ? 0 : 1);
    }
    if (a == 0x01FE) {
        ctx->fe_armed = 1;
    }
}

static uint8_t bus_rd_slow(ctx_t *ctx, uint32_t a, uint32_t c) {
    uint8_t v;
    a &= 0x1FFF;
    if (a & 0x1000) {
        v = cart_rd(ctx, a);
    } else if (a & 0x80) {
        v = (a & 0x200) ? riot_rd(ctx, a, c) : ctx->ram[a & 0x7F];
    } else {
        v = tia_rd(ctx, a, c);
    }
    if (ctx->bank_type == BK_FE) {
        fe_watch(ctx, a, v);
    }
    return v;
}

static uint32_t bus_wr_slow(ctx_t *ctx, uint32_t a, uint8_t v, uint32_t c) {
    a &= 0x1FFF;
    if (ctx->bank_type == BK_FE) {
        fe_watch(ctx, a, v);
    }
    if (a & 0x1000) {
        cart_wr(ctx, a, v);
    } else if (a & 0x80) {
        if (a & 0x200) {
            riot_wr(ctx, a, v, c);
        } else {
            ctx->ram[a & 0x7F] = v;
        }
    } else {
        if (ctx->bank_type == BK_3F && a < 0x40) {
            uint32_t b = v % (ctx->nbanks);
            ctx->slice[0] = ctx->rom + b * 2048;
            ctx->slice[1] = ctx->rom + b * 2048 + 1024;
        }
        return tia_wr(ctx, a, v, c);
    }
    return c + 1;
}

// Fast paths: ROM outside the hotspot/Superchip areas, and RIOT RAM. FE carts
// watch every access, so they always take the slow path.
static inline uint8_t bus_rd(ctx_t *ctx, uint32_t a, uint32_t c) {
    if (ctx->fast) {
        if (a & 0x1000) {
            uint32_t off = (a & 0xFFF) - ctx->fast_lo;
            if (off < 0xFE0 - ctx->fast_lo) {
                off += ctx->fast_lo;
                return ctx->slice[off >> 10][off & 0x3FF];
            }
        } else if ((a & 0x280) == 0x80) {
            return ctx->ram[a & 0x7F];
        }
    }
    return bus_rd_slow(ctx, a, c);
}

static inline uint32_t bus_wr(ctx_t *ctx, uint32_t a, uint8_t v, uint32_t c) {
    if (ctx->fast && (a & 0x1280) == 0x80) {
        ctx->ram[a & 0x7F] = v;
        return c + 1;
    }
    return bus_wr_slow(ctx, a, v, c);
}

static void cpu_run(ctx_t *ctx) {
    uint8_t A = ctx->A, X = ctx->X, Y = ctx->Y, S = ctx->S;
    uint16_t PC = ctx->PC;
    unsigned C_ = ctx->C_, V_ = ctx->V_, D_ = ctx->D_, I_ = ctx->I_, N_ = ctx->N_, Z_ = ctx->Z_;
    uint32_t cyc = ctx->cyc;
#define RD(a) bus_rd(ctx, (a), cyc++)
#define WR(a, v) (cyc = bus_wr(ctx, (a), (uint8_t)(v), cyc))
#define TICK() (cyc++)
#define JAM() (ctx->jammed = 1)
    while (!ctx->stop && !ctx->jammed) {
#include "cpu6502.h"
    }
#undef RD
#undef WR
#undef TICK
#undef JAM
    ctx->A = A; ctx->X = X; ctx->Y = Y; ctx->S = S; ctx->PC = PC;
    ctx->C_ = C_; ctx->V_ = V_; ctx->D_ = D_; ctx->I_ = I_; ctx->N_ = N_; ctx->Z_ = Z_;
    ctx->cyc = cyc;
}

// ============================================================================
// Entry points, shared by the host harness and the native module.

static void a26_set_input(ctx_t *ctx, uint32_t m) {
    uint8_t a = 0xFF;
    if (m & IN_RIGHT) a &= ~0x80;
    if (m & IN_LEFT) a &= ~0x40;
    if (m & IN_DOWN) a &= ~0x20;
    if (m & IN_UP) a &= ~0x10;
    ctx->swcha_in = a;
    uint8_t b = 0x08 | 0x03;
    if (m & IN_RESET) b &= ~0x01;
    if (m & IN_SELECT) b &= ~0x02;
    if (m & IN_BW) b &= ~0x08;
    if (m & IN_LEFT_A) b |= 0x40;
    if (m & IN_RIGHT_A) b |= 0x80;
    ctx->swchb_in = b;
    ctx->fire = (m & IN_FIRE) != 0;
}

static void a26_set_palette(ctx_t *ctx, uint32_t bpp) {
    for (int i = 0; i < 128; i++) {
        uint32_t c = ntsc_rgb[i];
        uint32_t r = c >> 16, g = (c >> 8) & 0xFF, b = c & 0xFF;
        if (bpp == 16) {
            uint32_t p = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            ctx->pal32[i] = p | (p << 16);
        } else {
            uint32_t p = ((r >> 5) << 5) | ((g >> 5) << 2) | (b >> 6);
            ctx->pal32[i] = p | (p << 8);
        }
    }
    col_update(ctx);
}

// Output geometry, see set_output() at the top. fb may be NULL (no drawing).
// The caller checks that the framebuffer is large enough.
static void a26_set_output(ctx_t *ctx, uint8_t *fb, uint32_t width, uint32_t height, uint32_t bpp, uint32_t top) {
    ctx->bpp = bpp;
    a26_set_palette(ctx, bpp);
    ctx->fb = fb;
    ctx->fb_width = width;
    ctx->fb_height = height;
    ctx->x0 = (width - 320) / 2;
    ctx->top = top;
    set_row(ctx);
}

static int a26_init(ctx_t *ctx, const uint8_t *rom, uint32_t rom_len) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->rom = rom;
    ctx->rom_len = rom_len;
    int e = cart_detect(ctx);
    if (e) {
        return e;
    }
    ctx->fast = ctx->bank_type != BK_FE;
    ctx->fast_lo = ctx->superchip ? 0x100 : 0;
    colltab_init(ctx);
    pf_update(ctx);
    prio_update(ctx);
    a26_set_input(ctx, 0);
    ctx->tim_start = 0;     // timer starts expired, free running
    ctx->tim_shift = 10;
    a26_set_output(ctx, NULL, 320, 240, 16, 12);
    ctx->crc = 0xFFFFFFFF;
    ctx->S = 0xFD;
    ctx->I_ = 1;
    ctx->Z_ = 1;
    uint32_t lo = bus_rd(ctx, 0x1FFC, 0);
    ctx->PC = (uint16_t)(lo | (bus_rd(ctx, 0x1FFD, 0) << 8));
    return 0;
}

// Drop whole lines from all cycle counters so they stay small.
static void rebase(ctx_t *ctx) {
    uint32_t k = ctx->line_clk;
    ctx->line_clk = 0;
    ctx->tclk -= k;
    ctx->cyc -= k / 3;
    ctx->tim_base -= (int32_t)(k / 3);
    if (ctx->tim_base < -0x10000000) {
        ctx->tim_base = -0x10000000;
    }
}

static uint32_t a26_run_frames(ctx_t *ctx, uint32_t n) {
    uint32_t i = 0;
    while (i < n && !ctx->jammed) {
        rebase(ctx);
        ctx->stop = 0;
        cpu_run(ctx);
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
    return mp_obj_new_int(a26_init(ctx, rom.buf, rom.len));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_init_obj, mod_init);

static mp_obj_t mod_mapper(mp_obj_t st_in) {
    char buf[5];
    size_t n = mapper_name(get_ctx(st_in), buf);
    return mp_obj_new_str(buf, n);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_mapper_obj, mod_mapper);

// set_output(st, fb, width, height, bpp, top); fb=None disables drawing.
static mp_obj_t mod_set_output(size_t n_args, const mp_obj_t *args) {
    ctx_t *ctx = get_ctx(args[0]);
    if (args[1] == mp_const_none) {
        ctx->fb = NULL;
        set_row(ctx);
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
    a26_set_output(ctx, fb.buf, width, height, bpp, top);
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
    a26_set_input(get_ctx(st_in), mp_obj_get_int(mask_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_set_input_obj, mod_set_input);

static mp_obj_t mod_run_frames(mp_obj_t st_in, mp_obj_t n_in) {
    return mp_obj_new_int(a26_run_frames(get_ctx(st_in), mp_obj_get_int(n_in)));
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
    mp_store_global(MP_QSTR_INPUT_FIRE, MP_OBJ_NEW_SMALL_INT(IN_FIRE));
    mp_store_global(MP_QSTR_INPUT_RESET, MP_OBJ_NEW_SMALL_INT(IN_RESET));
    mp_store_global(MP_QSTR_INPUT_SELECT, MP_OBJ_NEW_SMALL_INT(IN_SELECT));
    mp_store_global(MP_QSTR_INPUT_BW, MP_OBJ_NEW_SMALL_INT(IN_BW));
    mp_store_global(MP_QSTR_INPUT_LEFT_A, MP_OBJ_NEW_SMALL_INT(IN_LEFT_A));
    mp_store_global(MP_QSTR_INPUT_RIGHT_A, MP_OBJ_NEW_SMALL_INT(IN_RIGHT_A));
    mp_store_global(MP_QSTR_init, MP_OBJ_FROM_PTR(&mod_init_obj));
    mp_store_global(MP_QSTR_mapper, MP_OBJ_FROM_PTR(&mod_mapper_obj));
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
