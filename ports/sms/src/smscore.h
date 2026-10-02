// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Small Sega 8-bit core for microcontrollers: Master System, Game Gear and
// SG-1000. Z80 CPU (undocumented flags, IM 0/1/2, NMI, R register), VDP
// (SMS mode 4 and the TMS9918 modes), SN76489 PSG, Sega / Codemasters /
// plain cartridge mapping. NTSC timing only.
//
// Design, chosen for speed over accuracy:
// - The CPU runs whole instructions, counting T-states. I/O sees the cycle
//   count at the end of the instruction.
// - The VDP draws a whole scanline at the start of that line from the current
//   registers, then the CPU runs the line's 228 T-states. Writes made during a
//   line show up on the next one, which is what line interrupt handlers expect.
// - The PSG is caught up on each write and at the end of the frame, and
//   point-samples its channels at the output rate.
//
// The core allocates nothing and uses no writable static data. All state is in
// sms_t, which the caller owns. The ROM image is only read.
//
// Required from the includer: memset, memcpy, and a callback
//   static void sms_draw_line(sms_t *n, int line, const uint8_t *px);

// Unaligned 32-bit store (Cortex-M3 and up handle these in hardware).
typedef struct __attribute__((packed)) {
    uint32_t v;
} sms_u32u;
// which receives each rendered line as 256 color indices: 0..31 CRAM entries,
// 32..47 TMS9918 colors, possibly with bit 6 set (ignore it). The RGB565
// value of index i is n->rgb[i & 63]; n->rgb_ver changes when any does.

#define SMS_W 256
#define SMS_LINES 262
#define SMS_LINE_CYCLES 228
#define SMS_FRAME_CYCLES (SMS_LINES * SMS_LINE_CYCLES)  // 59736, 59.92 Hz
#define SMS_AUDIO_RATE 22050
// 368 samples at 22050 Hz is 59.92 fps, the NTSC frame rate.
#define SMS_AUDIO_SAMPLES 368

enum { SMS_SYS_SMS = 0, SMS_SYS_GG = 1, SMS_SYS_SG = 2, SMS_SYS_CPM = 3 };
enum { SMS_MAP_NONE = 0, SMS_MAP_SEGA = 1, SMS_MAP_CODIES = 2 };
enum { SMS_OK = 0, SMS_ERR_SIZE = 1 };

// Joypad bits (1 = pressed). Low six match port $DC.
#define SMS_UP 0x01
#define SMS_DOWN 0x02
#define SMS_LEFT 0x04
#define SMS_RIGHT 0x08
#define SMS_B1 0x10
#define SMS_B2 0x20
#define SMS_START 0x80  // Pause (NMI) on SMS and SG-1000, Start on Game Gear

typedef struct {
    // Z80
    uint16_t pc, sp, bc, de, hl, ix, iy, wz;
    uint16_t af2, bc2, de2, hl2;
    uint8_t a, f, i, r, r7, im, iff1, iff2;
    uint8_t halted, ei_block, nmi, irq;
    uint8_t stop;  // host test only: CP/M program exited
    int32_t cyc;   // T-states since the start of the frame

    // System
    uint8_t system, mapper, pad, pad_prev, ioctl, sram_used;
    uint8_t bank[4];           // Sega: FFFC..FFFF; Codemasters: slots 0..2
    const uint8_t *rom;
    uint32_t rom_size;
    uint32_t rom_pages;        // 1 KB pages
    uint8_t *cart_ram;         // caller's 32 KB, NULL = no cart RAM
    const uint8_t *rd[64];     // 1 KB read pages
    uint8_t *wr[64];           // 1 KB write pages, NULL = mapper handles it
    uint32_t frame;
    uint32_t instrs;           // unused, kept for the bench harness

    // VDP
    uint8_t reg[16];
    uint16_t addr;
    uint8_t code, latch, first, rbuf, status, cram_latch;
    uint8_t line_cnt, line_irq, vscroll, sp_tag;
    uint16_t height;           // active lines: 192, 224 or 240
    int32_t line;              // line being run
    uint32_t rgb_ver;
    uint16_t rgb[64];          // RGB565: 0..31 CRAM, 32..47 TMS palette
    uint8_t cram[64];

    // PSG
    uint16_t period[3];
    uint8_t vol[4], noise, psg_latch;
    uint8_t out[4], nflip;
    uint16_t lfsr;
    int32_t cnt[4];            // 16.16 tone clock ticks left
    int32_t dc;                // DC tracker for the high-pass
    uint32_t sample_ix;        // samples made this frame
    int16_t audio[SMS_AUDIO_SAMPLES];

    // Tables and memory
    uint8_t sz[256];           // S, Z, Y, X of a byte
    uint8_t szp[256];          // plus parity
    uint64_t expand[256];      // bit 7-i of a byte to byte lane i
    uint8_t line_px[SMS_W + 8];
    uint8_t sp_line[SMS_W];
    uint8_t vram[0x4000];
    uint8_t ram[0x2000];
} sms_t;

static void sms_draw_line(sms_t *n, int line, const uint8_t *px);

// TMS9918 palette as RGB565. Color 0 (transparent) shows as black.
static const uint16_t tms_rgb565[16] = {
    0x0000, 0x0000, 0x2648, 0x5EEF, 0x52BD, 0x7BBF, 0xD289, 0x475E,
    0xFAAA, 0xFBCF, 0xD60A, 0xE670, 0x2587, 0xCAD7, 0xCE79, 0xFFFF,
};

// 2 dB per step attenuation, scaled so four channels at full volume fit int16.
static const uint16_t psg_vol[16] = {
    8191, 6507, 5168, 4105, 3261, 2590, 2057, 1634, 1298, 1031, 819, 650, 517, 410, 326, 0,
};

// ---------------------------------------------------------------------------
// Memory map

static void map_rom16(sms_t *n, int first_page, int count, uint32_t bank) {
    for (int i = 0; i < count; i++) {
        uint32_t p = (bank * 16 + (first_page & 15) + i) % n->rom_pages;
        n->rd[first_page + i] = n->rom + p * 1024;
        n->wr[first_page + i] = NULL;
    }
}

static void map_update(sms_t *n) {
    if (n->mapper == SMS_MAP_SEGA) {
        map_rom16(n, 0, 1, 0);  // first 1 KB never pages
        map_rom16(n, 1, 15, n->bank[1]);
        map_rom16(n, 16, 16, n->bank[2]);
        if ((n->bank[0] & 0x08) && n->cart_ram) {
            uint8_t *base = n->cart_ram + ((n->bank[0] & 0x04) ? 0x4000 : 0);
            for (int i = 0; i < 16; i++) {
                n->rd[32 + i] = n->wr[32 + i] = base + i * 1024;
            }
            n->sram_used = 1;
        } else {
            map_rom16(n, 32, 16, n->bank[3]);
        }
    } else if (n->mapper == SMS_MAP_CODIES) {
        map_rom16(n, 0, 16, n->bank[0]);
        map_rom16(n, 16, 16, n->bank[1]);
        map_rom16(n, 32, 16, n->bank[2]);
        if ((n->bank[1] & 0x80) && n->cart_ram) {  // 8 KB RAM at A000 (Ernie Els Golf)
            for (int i = 0; i < 8; i++) {
                n->rd[40 + i] = n->wr[40 + i] = n->cart_ram + i * 1024;
            }
            n->sram_used = 1;
        }
    } else {
        for (int i = 0; i < 48; i++) {
            n->rd[i] = n->rom + (i % n->rom_pages) * 1024;
            n->wr[i] = NULL;
        }
    }
    for (int i = 48; i < 64; i++) {  // 8 KB RAM, mirrored
        n->rd[i] = n->wr[i] = n->ram + (i & 7) * 1024;
    }
}

static void mapper_write(sms_t *n, uint32_t ad, uint32_t v) {
    if (n->mapper == SMS_MAP_CODIES && (ad & 0x3FFF) == 0) {
        n->bank[ad >> 14] = v;
        map_update(n);
    }
}

static inline void mem_write(sms_t *n, uint32_t ad, uint32_t v) {
    uint8_t *p = n->wr[ad >> 10];
    if (p) {
        p[ad & 0x3FF] = v;
        if (ad >= 0xFFFC && n->mapper == SMS_MAP_SEGA) {
            n->bank[ad - 0xFFFC] = v;
            map_update(n);
        }
    } else {
        mapper_write(n, ad, v);
    }
}

// ---------------------------------------------------------------------------
// PSG (SN76489)

// Tone clock ticks (CPU clock / 16) per output sample, 16.16 fixed point.
#define PSG_STEP ((int32_t)(((int64_t)SMS_FRAME_CYCLES << 16) / (SMS_AUDIO_SAMPLES * 16)))

static void psg_run(sms_t *n, int32_t cyc) {
    uint32_t want = cyc > 0 ? (uint32_t)cyc * SMS_AUDIO_SAMPLES / SMS_FRAME_CYCLES : 0;
    if (want > SMS_AUDIO_SAMPLES) {
        want = SMS_AUDIO_SAMPLES;
    }
    while (n->sample_ix < want) {
        int32_t s = 0;
        for (int c = 0; c < 3; c++) {
            uint32_t p = n->period[c];
            if (p <= 1) {
                n->out[c] = 1;  // used for sample playback via volume writes
            } else {
                n->cnt[c] -= PSG_STEP;
                while (n->cnt[c] <= 0) {
                    n->cnt[c] += p << 16;
                    n->out[c] ^= 1;
                }
            }
            if (n->out[c]) {
                s += psg_vol[n->vol[c]];
            }
        }
        uint32_t np = (n->noise & 3) == 3 ? n->period[2] : (0x10u << (n->noise & 3));
        if (np == 0) {
            np = 1;
        }
        n->cnt[3] -= PSG_STEP;
        while (n->cnt[3] <= 0) {
            n->cnt[3] += np << 16;
            n->nflip ^= 1;
            if (n->nflip) {
                uint32_t l = n->lfsr;
                uint32_t fb = (n->noise & 4) ? ((l ^ (l >> 3)) & 1) : (l & 1);
                n->lfsr = (l >> 1) | (fb << 15);
                n->out[3] = n->lfsr & 1;
            }
        }
        if (n->out[3]) {
            s += psg_vol[n->vol[3]];
        }
        // High-pass: remove the DC of the unipolar channel sum (~7 Hz).
        n->dc += ((s << 8) - n->dc) >> 9;
        s -= n->dc >> 8;
        if (s > 32767) {
            s = 32767;
        } else if (s < -32768) {
            s = -32768;
        }
        n->audio[n->sample_ix++] = (int16_t)s;
    }
}

static void psg_write(sms_t *n, uint32_t v) {
    psg_run(n, n->cyc);
    if (v & 0x80) {
        n->psg_latch = (v >> 4) & 7;
    }
    uint32_t c = n->psg_latch >> 1;
    if (n->psg_latch & 1) {
        n->vol[c] = v & 0x0F;
    } else if (c < 3) {
        if (v & 0x80) {
            n->period[c] = (n->period[c] & 0x3F0) | (v & 0x0F);
        } else {
            n->period[c] = (n->period[c] & 0x0F) | ((v & 0x3F) << 4);
        }
    } else {
        n->noise = v & 7;
        n->lfsr = 0x8000;
    }
}

// ---------------------------------------------------------------------------
// VDP

static inline int vdp_mode4(sms_t *n) {
    return n->system != SMS_SYS_SG && (n->reg[0] & 0x04);
}

static void vdp_update_irq(sms_t *n) {
    n->irq = ((n->status & 0x80) && (n->reg[1] & 0x20)) ||
        (n->line_irq && (n->reg[0] & 0x10) && n->system != SMS_SYS_SG);
}

static void vdp_update_height(sms_t *n) {
    n->height = 192;
    if (vdp_mode4(n) && (n->reg[0] & 0x02) && n->system == SMS_SYS_SMS) {
        if (n->reg[1] & 0x10) {
            n->height = 224;
        } else if (n->reg[1] & 0x08) {
            n->height = 240;
        }
    }
}

static void cram_update(sms_t *n, int i) {
    uint32_t r, g, b;
    if (n->system == SMS_SYS_GG) {
        uint32_t c = n->cram[i * 2] | (n->cram[i * 2 + 1] << 8);
        r = (c & 15) * 17;
        g = ((c >> 4) & 15) * 17;
        b = ((c >> 8) & 15) * 17;
    } else {
        uint32_t c = n->cram[i];
        r = (c & 3) * 85;
        g = ((c >> 2) & 3) * 85;
        b = ((c >> 4) & 3) * 85;
    }
    n->rgb[i] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    n->rgb_ver++;
}

static void vdp_ctrl_write(sms_t *n, uint32_t v) {
    if (!n->latch) {
        n->first = v;
        n->addr = (n->addr & 0x3F00) | v;
        n->latch = 1;
        return;
    }
    n->latch = 0;
    n->code = v >> 6;
    n->addr = ((v & 0x3F) << 8) | n->first;
    if (n->code == 0) {
        n->rbuf = n->vram[n->addr];
        n->addr = (n->addr + 1) & 0x3FFF;
    } else if (n->code == 2 || (n->code == 3 && n->system == SMS_SYS_SG)) {
        n->reg[v & (n->system == SMS_SYS_SG ? 0x07 : 0x0F)] = n->first;
        vdp_update_height(n);
        vdp_update_irq(n);
    }
}

static void vdp_data_write(sms_t *n, uint32_t v) {
    n->latch = 0;
    if (n->code == 3 && n->system != SMS_SYS_SG) {
        if (n->system == SMS_SYS_GG) {
            if (!(n->addr & 1)) {
                n->cram_latch = v;
            } else {
                n->cram[(n->addr & 0x3E)] = n->cram_latch;
                n->cram[(n->addr & 0x3F)] = v;
                cram_update(n, (n->addr >> 1) & 31);
            }
        } else {
            n->cram[n->addr & 31] = v & 0x3F;
            cram_update(n, n->addr & 31);
        }
    } else {
        n->vram[n->addr] = v;
    }
    n->rbuf = v;
    n->addr = (n->addr + 1) & 0x3FFF;
}

static uint32_t vdp_data_read(sms_t *n) {
    n->latch = 0;
    uint32_t r = n->rbuf;
    n->rbuf = n->vram[n->addr];
    n->addr = (n->addr + 1) & 0x3FFF;
    return r;
}

static uint32_t vdp_ctrl_read(sms_t *n) {
    uint32_t r = n->status;
    if (n->system == SMS_SYS_SG) {
        n->status &= 0x1F;  // TMS9918 keeps the fifth sprite number
    } else {
        r |= 0x1F;
        n->status = 0;
    }
    n->line_irq = 0;
    n->latch = 0;
    vdp_update_irq(n);
    return r;
}

static uint32_t vdp_vcounter(sms_t *n) {
    int l = n->line;
    if (n->height == 224) {
        return l <= 0xEA ? l : l - 6;
    }
    if (n->height == 240) {
        return l & 0xFF;
    }
    return l <= 0xDA ? l : l - 6;
}

static uint32_t vdp_hcounter(sms_t *n) {
    int32_t c = n->cyc - n->line * SMS_LINE_CYCLES;
    if (c < 0) {
        c = 0;
    }
    uint32_t h = (uint32_t)(c * 3) >> 2;  // 342 dots / 2 per 228 T-states
    if (h > 0x93) {
        h += 0x55;
    }
    return h & 0xFF;
}

// Next sprite-line tag; clears the owner array when it wraps.
static uint8_t next_tag(sms_t *n) {
    if (++n->sp_tag == 0) {
        memset(n->sp_line, 0, sizeof(n->sp_line));
        n->sp_tag = 1;
    }
    return n->sp_tag;
}

static void render_mode4(sms_t *n, int line) {
    uint8_t *lb = n->line_px;
    const uint8_t *reg = n->reg;
    const uint8_t *vram = n->vram;
    if (!(reg[1] & 0x40)) {
        memset(lb, 16 + (reg[7] & 15), SMS_W);
        return;
    }
    uint32_t ntbase;
    uint32_t row;
    if (n->height == 192) {
        ntbase = (reg[2] & 0x0E) << 10;
        row = (line + n->vscroll) % 224;
    } else {
        ntbase = ((reg[2] & 0x0C) << 10) | 0x700;
        row = (line + n->vscroll) & 255;
    }
    uint32_t hs = ((reg[0] & 0x40) && line < 16) ? 0 : reg[8];
    uint32_t fine = hs & 7;
    uint32_t coarse = hs >> 3;
    uint8_t *dst = lb + fine;
    for (uint32_t s = 0; s < 32; s++, dst += 8) {
        uint32_t y = (s >= 24 && (reg[0] & 0x80)) ? (uint32_t)line : row;
        uint32_t col = (s - coarse) & 31;
        uint32_t ea = (ntbase + (y >> 3) * 64 + col * 2) & 0x3FFF;
        uint32_t e = vram[ea] | (vram[ea + 1] << 8);
        uint32_t r = (e & 0x400) ? 7 - (y & 7) : (y & 7);
        const uint8_t *pat = vram + (((e & 0x1FF) << 5) | (r << 2));
        uint64_t px = n->expand[pat[0]] | (n->expand[pat[1]] << 1) |
            (n->expand[pat[2]] << 2) | (n->expand[pat[3]] << 3);
        if (e & 0x200) {
            px = __builtin_bswap64(px);
        }
        if (e & 0x1000) {
            px |= ((px | (px >> 1) | (px >> 2) | (px >> 3)) & 0x0101010101010101ull) << 6;
        }
        if (e & 0x800) {
            px |= 0x1010101010101010ull;
        }
        ((sms_u32u *)dst)[0].v = (uint32_t)px;
        ((sms_u32u *)dst)[1].v = (uint32_t)(px >> 32);
    }
    for (uint32_t i = 0; i < fine; i++) {
        lb[i] = lb[SMS_W + i];
    }

    // Sprites
    const uint8_t *sat = vram + ((reg[5] & 0x7E) << 7);
    uint32_t zoom = reg[1] & 1;
    uint32_t h = ((reg[1] & 2) ? 16 : 8) << zoom;
    uint32_t tile_hi = (reg[6] & 4) << 6;
    int xshift = (reg[0] & 8) ? 8 : 0;
    uint8_t list[8], dys[8];
    int count = 0;
    for (int i = 0; i < 64; i++) {
        uint32_t y = sat[i];
        if (y == 0xD0 && n->height == 192) {
            break;
        }
        uint32_t dy = (uint32_t)(line - (int)y - 1) & 0xFF;
        if (dy >= h) {
            continue;
        }
        if (count == 8) {
            n->status |= 0x40;
            break;
        }
        list[count] = i;
        dys[count++] = dy;
    }
    if (count) {
        uint8_t tag = next_tag(n);
        uint8_t *own = n->sp_line;
        for (int k = 0; k < count; k++) {
            int i = list[k];
            int x = sat[0x80 + i * 2] - xshift;
            uint32_t tile = sat[0x81 + i * 2] | tile_hi;
            uint32_t row8 = dys[k] >> zoom;
            if (reg[1] & 2) {
                tile = (tile & ~1u) + (row8 >> 3);
            }
            const uint8_t *pat = vram + ((tile << 5) | ((row8 & 7) << 2));
            uint64_t px = n->expand[pat[0]] | (n->expand[pat[1]] << 1) |
                (n->expand[pat[2]] << 2) | (n->expand[pat[3]] << 3);
            if (!px) {
                continue;
            }
            for (int b = 0; b < 8; b++, px >>= 8) {
                uint32_t c = px & 15;
                if (!c) {
                    continue;
                }
                for (int z = 0; z <= (int)zoom; z++) {
                    int sx = x + (b << zoom) + z;
                    if (sx < 0 || sx >= SMS_W) {
                        continue;
                    }
                    if (own[sx] == tag) {
                        n->status |= 0x20;
                        continue;
                    }
                    own[sx] = tag;
                    if (!(lb[sx] & 0x40)) {
                        lb[sx] = c | 16;
                    }
                }
            }
        }
    }
    if (reg[0] & 0x20) {
        memset(lb, 16 + (reg[7] & 15), 8);
    }
}

static void render_tms(sms_t *n, int line) {
    uint8_t *lb = n->line_px;
    const uint8_t *reg = n->reg;
    const uint8_t *vram = n->vram;
    uint32_t bd = 32 + (reg[7] & 15);
    if (!(reg[1] & 0x40)) {
        memset(lb, bd, SMS_W);
        return;
    }
    uint32_t nt = (reg[2] & 0x0F) << 10;
    uint32_t pg = (reg[4] & 7) << 11;
    uint32_t row = line >> 3;
    if (reg[1] & 0x10) {  // text: 40 x 6 px columns, 8 px borders
        uint32_t fg = reg[7] >> 4 ? 32 + (reg[7] >> 4) : bd;
        memset(lb, bd, SMS_W);
        for (uint32_t c = 0; c < 40; c++) {
            uint32_t p = vram[pg + vram[(nt + row * 40 + c) & 0x3FFF] * 8 + (line & 7)];
            for (int b = 0; b < 6; b++) {
                lb[8 + c * 6 + b] = (p & (0x80 >> b)) ? fg : bd;
            }
        }
        return;  // no sprites in text mode
    }
    for (uint32_t c = 0; c < 32; c++) {
        uint32_t name = vram[nt + row * 32 + c];
        uint32_t p, col;
        if (reg[1] & 0x08) {  // multicolor
            p = 0xF0;
            col = vram[(pg + name * 8 + (row & 3) * 2 + ((line >> 2) & 1)) & 0x3FFF];
        } else if (reg[0] & 0x02) {  // graphics II
            uint32_t idx = ((name + ((line >> 6) << 8)) << 3) | (line & 7);
            p = vram[(0xFFFFE000u | idx) & ((reg[4] << 11) | 0x7FF) & 0x3FFF];
            col = vram[(0xFFFFE000u | idx) & ((reg[3] << 6) | 0x3F) & 0x3FFF];
        } else {  // graphics I
            p = vram[pg + name * 8 + (line & 7)];
            col = vram[(reg[3] << 6) + (name >> 3)];
        }
        uint32_t fg = (col >> 4) ? 32 + (col >> 4) : bd;
        uint32_t bg = (col & 15) ? 32 + (col & 15) : bd;
        uint8_t *d = lb + c * 8;
        for (int b = 0; b < 8; b++) {
            d[b] = (p & (0x80 >> b)) ? fg : bg;
        }
    }

    // Sprites: 32, four per line, 8x8 or 16x16, optionally magnified.
    const uint8_t *sat = vram + ((reg[5] & 0x7F) << 7);
    uint32_t spg = (reg[6] & 7) << 11;
    uint32_t size = (reg[1] & 2) ? 16 : 8;
    uint32_t mag = reg[1] & 1;
    int count = 0;
    uint8_t tag = 0;
    uint8_t *own = n->sp_line;
    for (int i = 0; i < 32; i++) {
        const uint8_t *s = sat + i * 4;
        if (s[0] == 0xD0) {
            break;
        }
        int sy = s[0] >= 0xE1 ? s[0] - 255 : s[0] + 1;
        int dy = line - sy;
        if (dy < 0 || dy >= (int)(size << mag)) {
            continue;
        }
        if (count == 4) {
            if (!(n->status & 0x40)) {
                n->status = (n->status & 0xA0) | 0x40 | i;
            }
            break;
        }
        count++;
        if (!tag) {
            tag = next_tag(n);
        }
        int x = s[1] - ((s[3] & 0x80) ? 32 : 0);
        uint32_t c = s[3] & 15;
        uint32_t name = size == 16 ? (s[2] & 0xFC) : s[2];
        uint32_t r = dy >> mag;
        const uint8_t *pat = vram + ((spg + name * 8 + r) & 0x3FFF);
        uint32_t bits = pat[0] << 8;
        if (size == 16) {
            bits |= pat[16];
        }
        for (uint32_t b = 0; b < size; b++) {
            if (!(bits & (0x8000 >> b))) {
                continue;
            }
            for (uint32_t z = 0; z <= mag; z++) {
                int sx = x + (int)((b << mag) + z);
                if (sx < 0 || sx >= SMS_W) {
                    continue;
                }
                if (own[sx] == tag) {
                    n->status |= 0x20;
                    continue;
                }
                own[sx] = tag;
                if (c) {
                    lb[sx] = 32 + c;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// I/O ports

static uint32_t io_in(sms_t *n, uint32_t port) {
    port &= 0xFF;
    if (n->system == SMS_SYS_GG && port <= 6) {
        static const uint8_t gg_regs[7] = {0xC0, 0x7F, 0xFF, 0x00, 0xFF, 0x00, 0xFF};
        if (port == 0) {
            return (n->pad & SMS_START) ? 0x40 : 0xC0;  // export, NTSC
        }
        return gg_regs[port];
    }
    switch (port & 0xC1) {
        case 0x40:
            return vdp_vcounter(n);
        case 0x41:
            return vdp_hcounter(n);
        case 0x80:
            return vdp_data_read(n);
        case 0x81:
            return vdp_ctrl_read(n);
        case 0xC0:
            return (~n->pad & 0x3F) | 0xC0;
        case 0xC1: {
            // Reset button up; TH lines read back what the I/O control port
            // drives them to (export console).
            uint32_t v = 0x3F;
            v |= (n->ioctl & 0x02) ? 0x40 : (n->ioctl & 0x20) << 1;
            v |= (n->ioctl & 0x08) ? 0x80 : (n->ioctl & 0x80);
            return v;
        }
        default:
            return 0xFF;
    }
}

#ifdef SMS_CPM_HOOK
static void cpm_out(sms_t *n, uint32_t port, uint32_t v);
#endif

static void io_out(sms_t *n, uint32_t port, uint32_t v) {
#ifdef SMS_CPM_HOOK
    if (n->system == SMS_SYS_CPM) {
        cpm_out(n, port & 0xFF, v);
        return;
    }
#endif
    port &= 0xFF;
    if (n->system == SMS_SYS_GG && port <= 6) {
        return;  // serial port and stereo control: ignored
    }
    switch (port & 0xC1) {
        case 0x01:
            n->ioctl = v;
            break;
        case 0x40:
        case 0x41:
            psg_write(n, v);
            break;
        case 0x80:
            vdp_data_write(n, v);
            break;
        case 0x81:
            vdp_ctrl_write(n, v);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Z80

#define RD(ad) (n->rd[(ad) >> 10][(ad) & 0x3FF])
#define RD16(ad) (RD(ad) | (RD(((ad) + 1) & 0xFFFF) << 8))
#define WR(ad, v) mem_write(n, (ad), (v))
#define WR16(ad, v) { uint32_t _wa = (ad), _wv = (v); WR(_wa, _wv & 0xFF); WR((_wa + 1) & 0xFFFF, _wv >> 8); }
#define FETCH_M1() (n->r++, _ft = RD(pc), pc = (pc + 1) & 0xFFFF, _ft)
#define PUSH(v) { uint32_t _pv = (v); sp = (sp - 1) & 0xFFFF; WR(sp, _pv >> 8); \
                  sp = (sp - 1) & 0xFFFF; WR(sp, _pv & 0xFF); }
#define POP() ({ uint32_t _v = RD16(sp); sp = (sp + 2) & 0xFFFF; _v; })

#ifdef SMS_CPM_HOOK
#define SYNC_OUT() (n->a = a, n->f = f, n->bc = bc, n->de = de, n->hl = hl, n->sp = sp, n->pc = pc)
#define IO_STOP() if (n->stop) { goto z80_done; }
#else
#define SYNC_OUT() ((void)0)
#define IO_STOP()
#endif
#define IO_IN(port) ({ n->cyc = cyc; io_in(n, (port)); })
#define IO_OUT(port, v) { n->cyc = cyc; SYNC_OUT(); io_out(n, (port), (v)); IO_STOP(); }

#define ADD8(val) { uint32_t _v = (val), _r = a + _v; \
    f = n->sz[_r & 0xFF] | (_r >> 8) | ((a ^ _v ^ _r) & 0x10) | (((a ^ _r) & (_v ^ _r) & 0x80) >> 5); a = _r & 0xFF; }
#define ADC8(val) { uint32_t _v = (val), _r = a + _v + (f & 1); \
    f = n->sz[_r & 0xFF] | (_r >> 8) | ((a ^ _v ^ _r) & 0x10) | (((a ^ _r) & (_v ^ _r) & 0x80) >> 5); a = _r & 0xFF; }
#define SUB8(val) { uint32_t _v = (val), _r = a - _v; \
    f = n->sz[_r & 0xFF] | ((_r >> 8) & 1) | 2 | ((a ^ _v ^ _r) & 0x10) | (((a ^ _v) & (a ^ _r) & 0x80) >> 5); a = _r & 0xFF; }
#define SBC8(val) { uint32_t _v = (val), _r = a - _v - (f & 1); \
    f = n->sz[_r & 0xFF] | ((_r >> 8) & 1) | 2 | ((a ^ _v ^ _r) & 0x10) | (((a ^ _v) & (a ^ _r) & 0x80) >> 5); a = _r & 0xFF; }
#define CP8(val) { uint32_t _v = (val), _r = a - _v; \
    f = (n->sz[_r & 0xFF] & 0xD7) | (_v & 0x28) | ((_r >> 8) & 1) | 2 | ((a ^ _v ^ _r) & 0x10) | \
        (((a ^ _v) & (a ^ _r) & 0x80) >> 5); }
#define AND8(val) { a &= (val); f = n->szp[a] | 0x10; }
#define XOR8(val) { a ^= (val); f = n->szp[a]; }
#define OR8(val) { a |= (val); f = n->szp[a]; }
#define INC8(v) { v = (v + 1) & 0xFF; f = (f & 1) | n->sz[v] | ((v & 0xF) ? 0 : 0x10) | (v == 0x80 ? 4 : 0); }
#define DEC8(v) { v = (v - 1) & 0xFF; f = (f & 1) | 2 | n->sz[v] | ((v & 0xF) == 0xF ? 0x10 : 0) | (v == 0x7F ? 4 : 0); }
#define RLC(v) { v = ((v << 1) | (v >> 7)) & 0xFF; f = n->szp[v] | (v & 1); }
#define RRC(v) { uint32_t _c = v & 1; v = (v >> 1) | (_c << 7); f = n->szp[v] | _c; }
#define RL(v) { uint32_t _c = v >> 7; v = ((v << 1) | (f & 1)) & 0xFF; f = n->szp[v] | _c; }
#define RR(v) { uint32_t _c = v & 1; v = (v >> 1) | ((f & 1) << 7); f = n->szp[v] | _c; }
#define SLA(v) { uint32_t _c = v >> 7; v = (v << 1) & 0xFF; f = n->szp[v] | _c; }
#define SRA(v) { uint32_t _c = v & 1; v = (v >> 1) | (v & 0x80); f = n->szp[v] | _c; }
#define SLL(v) { uint32_t _c = v >> 7; v = ((v << 1) | 1) & 0xFF; f = n->szp[v] | _c; }
#define SRL(v) { uint32_t _c = v & 1; v >>= 1; f = n->szp[v] | _c; }
#define BIT(b, val, xysrc) { uint32_t _t = (val) & (1 << (b)); \
    f = (f & 1) | 0x10 | (_t ? (_t & 0x80) : 0x44) | ((xysrc) & 0x28); }
#define ADD16(dst, val) { uint32_t _v = (val), _r = dst + _v; wz = (dst + 1) & 0xFFFF; \
    f = (f & 0xC4) | ((_r >> 16) & 1) | ((_r >> 8) & 0x28) | (((dst ^ _v ^ _r) >> 8) & 0x10); dst = _r & 0xFFFF; }
#define ADC16(val) { uint32_t _v = (val), _r = hl + _v + (f & 1); wz = (hl + 1) & 0xFFFF; \
    f = ((_r >> 16) & 1) | ((_r >> 8) & 0xA8) | ((_r & 0xFFFF) ? 0 : 0x40) | (((hl ^ _v ^ _r) >> 8) & 0x10) | \
        (((hl ^ _r) & (_v ^ _r) & 0x8000) >> 13); hl = _r & 0xFFFF; }
#define SBC16(val) { uint32_t _v = (val), _r = hl - _v - (f & 1); wz = (hl + 1) & 0xFFFF; \
    f = ((_r >> 16) & 1) | 2 | ((_r >> 8) & 0xA8) | ((_r & 0xFFFF) ? 0 : 0x40) | (((hl ^ _v ^ _r) >> 8) & 0x10) | \
        (((hl ^ _v) & (hl ^ _r) & 0x8000) >> 13); hl = _r & 0xFFFF; }
#define RLCA() { a = ((a << 1) | (a >> 7)) & 0xFF; f = (f & 0xC4) | (a & 0x29); }
#define RRCA() { uint32_t _c = a & 1; a = (a >> 1) | (_c << 7); f = (f & 0xC4) | (a & 0x28) | _c; }
#define RLA() { uint32_t _c = a >> 7; a = ((a << 1) | (f & 1)) & 0xFF; f = (f & 0xC4) | (a & 0x28) | _c; }
#define RRA() { uint32_t _c = a & 1; a = (a >> 1) | ((f & 1) << 7); f = (f & 0xC4) | (a & 0x28) | _c; }
#define DAA() { uint32_t _t = a, _d = 0, _c = f & 1; \
    if ((f & 0x10) || (a & 0x0F) > 9) _d = 6; \
    if (_c || a > 0x99) { _d |= 0x60; _c = 1; } \
    a = ((f & 2) ? a - _d : a + _d) & 0xFF; \
    f = n->szp[a] | _c | (f & 2) | ((_t ^ a) & 0x10); }
#define CPL() { a ^= 0xFF; f = (f & 0xC5) | (a & 0x28) | 0x12; }
#define SCF() { f = (f & 0xC4) | (a & 0x28) | 1; }
#define CCF() { f = ((f & 0xC5) | ((f & 1) << 4) | (a & 0x28)) ^ 1; }
#define RLD() { uint32_t _m = RD(hl); WR(hl, ((_m << 4) | (a & 0x0F)) & 0xFF); a = (a & 0xF0) | (_m >> 4); \
    f = (f & 1) | n->szp[a]; wz = (hl + 1) & 0xFFFF; }
#define RRD() { uint32_t _m = RD(hl); WR(hl, ((a << 4) | (_m >> 4)) & 0xFF); a = (a & 0xF0) | (_m & 0x0F); \
    f = (f & 1) | n->szp[a]; wz = (hl + 1) & 0xFFFF; }
#define LDI_OP(d) { v = RD(hl); WR(de, v); hl = (hl + (d)) & 0xFFFF; de = (de + (d)) & 0xFFFF; \
    bc = (bc - 1) & 0xFFFF; uint32_t _t = v + a; \
    f = (f & 0xC1) | (bc ? 4 : 0) | (_t & 0x08) | ((_t << 4) & 0x20); }
#define CPI_OP(d) { v = RD(hl); uint32_t _r = (a - v) & 0xFF, _h = (a ^ v ^ _r) & 0x10; \
    hl = (hl + (d)) & 0xFFFF; bc = (bc - 1) & 0xFFFF; wz = (wz + (d)) & 0xFFFF; \
    uint32_t _t = _r - (_h >> 4); \
    f = (f & 1) | 2 | (n->sz[_r] & 0xC0) | _h | (bc ? 4 : 0) | (_t & 8) | ((_t << 4) & 0x20); }
#define BLOCK_IO_FLAGS(k) { uint32_t _b = bc >> 8; \
    f = n->sz[_b] | ((v >> 6) & 2) | ((k) > 255 ? 0x11 : 0) | (n->szp[((k) & 7) ^ _b] & 4); }
#define INI_OP(d) { v = IO_IN(bc); WR(hl, v); hl = (hl + (d)) & 0xFFFF; wz = (bc + (d)) & 0xFFFF; \
    uint32_t _k = v + (((bc & 0xFF) + (d)) & 0xFF); bc = (bc - 0x100) & 0xFFFF; BLOCK_IO_FLAGS(_k); }
#define OUTI_OP(d) { v = RD(hl); bc = (bc - 0x100) & 0xFFFF; IO_OUT(bc, v); hl = (hl + (d)) & 0xFFFF; \
    wz = (bc + (d)) & 0xFFFF; uint32_t _k = v + (hl & 0xFF); BLOCK_IO_FLAGS(_k); }
#define HALT_OP() { n->halted = 1; pc = (pc - 1) & 0xFFFF; \
    if (!(n->nmi | (n->irq & n->iff1)) && cyc < target) { \
        int32_t _k = (target - cyc + 3) >> 2; cyc += _k << 2; n->r += _k; } }
#define EI_OP() { if (cyc < target) goto fetch; n->ei_block = 1; }

// Run until n->cyc reaches target.
static void z80_run(sms_t *n, int32_t target) {
    uint32_t a = n->a, f = n->f, bc = n->bc, de = n->de, hl = n->hl;
    uint32_t sp = n->sp, pc = n->pc, wz = n->wz;
    int32_t cyc = n->cyc;
    uint32_t op, v, ad, _ft;
    if (cyc >= target) {
        return;
    }
    if (n->ei_block) {
        n->ei_block = 0;
        goto fetch;
    }
    while (cyc < target) {
        if (n->nmi | (n->irq & n->iff1)) {
            if (n->halted) {
                n->halted = 0;
                pc = (pc + 1) & 0xFFFF;
            }
            n->r++;
            if (n->nmi) {
                n->nmi = 0;
                n->iff1 = 0;
                PUSH(pc);
                pc = 0x66;
                cyc += 11;
            } else {
                n->iff1 = n->iff2 = 0;
                PUSH(pc);
                if (n->im == 2) {
                    ad = (n->i << 8) | 0xFF;
                    pc = RD16(ad);
                    cyc += 19;
                } else {
                    pc = 0x38;
                    cyc += 13;
                }
            }
            wz = pc;
            continue;
        }
    fetch:
        op = FETCH_M1();
    dispatch:
        switch (op) {
            #include "z80_ops.inc"
        }
    }
    goto z80_done;  // keeps the label used in all builds
z80_done:
    n->a = a;
    n->f = f;
    n->bc = bc;
    n->de = de;
    n->hl = hl;
    n->sp = sp;
    n->pc = pc;
    n->wz = wz;
    n->cyc = cyc;
}

// ---------------------------------------------------------------------------
// System

static void sms_tables(sms_t *n) {
    for (int i = 0; i < 256; i++) {
        uint32_t p = i ^ (i >> 4);
        p ^= p >> 2;
        p ^= p >> 1;
        n->sz[i] = (i & 0xA8) | (i ? 0 : 0x40);
        n->szp[i] = n->sz[i] | ((p & 1) ? 0 : 4);
        uint64_t e = 0;
        for (int b = 0; b < 8; b++) {
            if (i & (0x80 >> b)) {
                e |= 1ull << (b * 8);
            }
        }
        n->expand[i] = e;
    }
    for (int i = 0; i < 16; i++) {
        n->rgb[32 + i] = tms_rgb565[i];
    }
}

// Codemasters carts carry a checksum pair at 0x7FE6/0x7FE8 that sums to 0x10000.
static int is_codemasters(const uint8_t *rom, uint32_t len) {
    if (len < 0x10000) {
        return 0;
    }
    uint32_t c = rom[0x7FE6] | (rom[0x7FE7] << 8);
    uint32_t k = rom[0x7FE8] | (rom[0x7FE9] << 8);
    return c != 0 && c + k == 0x10000;
}

static int sms_init(sms_t *n, const uint8_t *rom, uint32_t len, int system) {
    memset(n, 0, sizeof(*n));
    if ((len & 0x3FFF) == 512) {  // copier header
        rom += 512;
        len -= 512;
    }
    if (len < 1024) {
        return SMS_ERR_SIZE;
    }
    n->system = system;
    n->rom = rom;
    n->rom_size = len;
    n->rom_pages = len >> 10;
    sms_tables(n);
    if (system == SMS_SYS_SG) {
        n->mapper = SMS_MAP_NONE;
    } else if (is_codemasters(rom, len)) {
        n->mapper = SMS_MAP_CODIES;
        n->bank[0] = 0;
        n->bank[1] = 1;
        n->bank[2] = 0;
    } else {
        n->mapper = SMS_MAP_SEGA;
        n->bank[0] = 0;
        n->bank[1] = 0;
        n->bank[2] = 1;
        n->bank[3] = 2;
    }
    map_update(n);
    n->sp = 0xDFF0;
    n->a = n->f = 0xFF;
    n->im = 1;
    n->height = 192;
    n->lfsr = 0x8000;
    for (int c = 0; c < 4; c++) {
        n->vol[c] = 15;
    }
    for (int i = 0; i < 32; i++) {
        cram_update(n, i);
    }
    return SMS_OK;
}

static void sms_set_pad(sms_t *n, uint8_t pad) {
    if ((pad & ~n->pad_prev & SMS_START) && n->system != SMS_SYS_GG) {
        n->nmi = 1;
    }
    n->pad = n->pad_prev = pad;
}

// True for lines the frontend shows (Game Gear shows lines 24..167 only).
static inline int line_visible(sms_t *n, int line) {
    if (n->system == SMS_SYS_GG) {
        return line >= 24 && line < 168;
    }
    return line < n->height;
}

static void sms_run_frame(sms_t *n) {
    n->sample_ix = 0;
    n->vscroll = n->reg[9];
    for (int line = 0; line < SMS_LINES; line++) {
        n->line = line;
        if (line < n->height && line_visible(n, line)) {
            if (vdp_mode4(n)) {
                render_mode4(n, line);
            } else {
                render_tms(n, line);
            }
            sms_draw_line(n, line, n->line_px);
        }
        if (line <= n->height) {
            if (n->line_cnt-- == 0) {
                n->line_cnt = n->reg[10];
                n->line_irq = 1;
            }
        } else {
            n->line_cnt = n->reg[10];
        }
        if (line == n->height + 1) {
            n->status |= 0x80;
        }
        vdp_update_irq(n);
        z80_run(n, (line + 1) * SMS_LINE_CYCLES);
    }
    psg_run(n, SMS_FRAME_CYCLES);
    n->cyc -= SMS_FRAME_CYCLES;
    n->frame++;
}

#ifdef SMS_CPM_HOOK
// Host test only: flat 64 KB RAM, no VDP, I/O goes to cpm_out().
static void sms_init_flat(sms_t *n, uint8_t *mem) {
    memset(n, 0, sizeof(*n));
    n->system = SMS_SYS_CPM;
    sms_tables(n);
    for (int i = 0; i < 64; i++) {
        n->rd[i] = n->wr[i] = mem + i * 1024;
    }
}
#endif
