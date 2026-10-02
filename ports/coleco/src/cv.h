// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// ColecoVision machine, written for this project: Z80 (z80.h), TMS9918A VDP
// drawn a scanline at a time, SN76489 PSG, controllers, MegaCart banking.
//
// Memory: $0000-$1FFF BIOS, $6000-$7FFF 1 KB RAM mirrored, $8000-$FFFF cart.
// I/O (low address byte): $80-$9F keypad mode, $C0-$DF joystick mode,
// $A0-$BF VDP (even data, odd control), $E0-$FF PSG write / controller read
// (A1 picks the controller).
//
// Timing: 228 CPU cycles per line, 262 lines, 59736 cycles (59.92 Hz) a frame.
// Each visible line is drawn at the start of its 228 cycles from the current
// VRAM and registers. The frame flag is set at the start of line 192, and the
// VDP interrupt output drives the Z80 NMI (edge triggered).
#ifndef CV_MACHINE_H
#define CV_MACHINE_H

#include <stdint.h>
#include <string.h>

#define CV_W 256
#define CV_H 192
#define CV_LINE_CYC 228
#define CV_LINES 262
#define CV_FRAME_CYC (CV_LINE_CYC * CV_LINES)
#define CV_AUDIO_RATE 22050
#define CV_AUDIO_SAMPLES 368  // 22050 / 59.92
#define CV_PSG_STEP 664934  // (3579545 / 16 / 22050) in 16.16

// Input mask for one controller, 1 = pressed.
#define CV_UP 0x01
#define CV_RIGHT 0x02
#define CV_DOWN 0x04
#define CV_LEFT 0x08
#define CV_FIRE_L 0x10
#define CV_FIRE_R 0x20
#define CV_KEY(n) (0x100u << (n))  // 0-9, 10 is *, 11 is #

struct cv_s;
static inline uint8_t cv_rd(struct cv_s *m, uint16_t a);
static inline void cv_wr(struct cv_s *m, uint16_t a, uint8_t v);
static uint8_t cv_in(struct cv_s *m, uint16_t port);
static void cv_out(struct cv_s *m, uint16_t port, uint8_t v);

#define Z80_RD(a) cv_rd((struct cv_s *)z, (a))
#define Z80_WR(a, v) cv_wr((struct cv_s *)z, (a), (v))
#define Z80_IN(p) cv_in((struct cv_s *)z, (p))
#define Z80_OUT(p, v) cv_out((struct cv_s *)z, (p), (v))
#include "z80.h"
#include "cv_bios.h"

typedef struct cv_s {
    z80_t cpu;  // first, so z80_t * and cv_t * are the same pointer
    const uint8_t *bios;
    const uint8_t *rom;
    uint32_t rom_size;
    const uint8_t *page[4];  // 8 KB pages at $8000, $A000, $C000, $E000
    uint32_t banks;  // MegaCart 16 KB bank count, 0 for plain carts
    uint32_t frame;
    uint32_t input[2];
    // VDP
    uint8_t reg[8];
    uint8_t status;
    uint8_t latch_on;
    uint8_t latch;
    uint8_t rdbuf;
    uint16_t addr;
    uint8_t int_line;
    uint8_t joy_mode;  // 1 after a write to $C0, 0 after $80
    // PSG
    uint16_t period[3];
    uint8_t vol[4];
    uint8_t noise_ctl;
    uint8_t psg_latch;
    uint8_t out[4];
    uint16_t lfsr;
    int32_t cnt[4];
    uint32_t audio_pos;
    int16_t audio[CV_AUDIO_SAMPLES];
    // Output
    uint8_t *fb;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t bpp;
    uint32_t x0;
    uint32_t y0;
    uint32_t border;  // backdrop color the border was last drawn in, 0xFF = redraw
    uint32_t crc_on;
    uint32_t crc;
    uint32_t last_crc;
    int16_t *ring;
    uint32_t ring_frames;
    uint32_t ring_pos;
    uint16_t lut16[16];
    uint8_t lut8[16];
    uint8_t line[CV_W];
    uint8_t occ[CV_W];
    uint8_t ram[1024];
    uint8_t vram[16384];
} cv_t;

// TMS9918A colors (commonly used measured values); 0 is transparent.
static const uint16_t cv_pal565[16] = {
    0x0000, 0x0000, 0x2648, 0x5EEF, 0x52BD, 0x7BBF, 0xD289, 0x475E,
    0xFAAA, 0xFBCF, 0xD60A, 0xE670, 0x2587, 0xCAD7, 0xCE79, 0xFFFF,
};
static const uint8_t cv_pal332[16] = {
    0x00, 0x00, 0x39, 0x59, 0x4B, 0x6F, 0xC9, 0x5F, 0xE9, 0xED, 0xD9, 0xFA, 0x34, 0xCA, 0xDB, 0xFF,
};
// 2 dB steps, 15 is off.
static const int16_t cv_vol[16] = {
    4096, 3254, 2584, 2053, 1631, 1295, 1029, 817, 649, 516, 410, 325, 258, 205, 163, 0,
};
// Keypad codes on bits 0-3 for keys 0-9, *, #.
static const uint8_t cv_keycode[12] = {
    0x0A, 0x0D, 0x07, 0x0C, 0x02, 0x03, 0x0E, 0x05, 0x01, 0x0B, 0x09, 0x06,
};

static void cv_bank(cv_t *m, uint16_t a) {
    uint32_t b = (a & 0x3F) % m->banks;
    m->page[2] = m->rom + (b << 14);
    m->page[3] = m->page[2] + 0x2000;
}

static inline uint8_t cv_rd(cv_t *m, uint16_t a) {
    if (a & 0x8000) {
        uint8_t v = m->page[(a >> 13) & 3][a & 0x1FFF];
        if (a >= 0xFFC0 && m->banks) {
            cv_bank(m, a);
        }
        return v;
    }
    if (a < 0x2000) {
        return m->bios[a];
    }
    if (a >= 0x6000) {
        return m->ram[a & 0x3FF];
    }
    return 0xFF;
}

static inline void cv_wr(cv_t *m, uint16_t a, uint8_t v) {
    if ((a & 0xE000) == 0x6000) {
        m->ram[a & 0x3FF] = v;
    }
}

// ---- VDP ---------------------------------------------------------------------

static void vdp_update_int(cv_t *m) {
    uint8_t line = (m->status & 0x80) && (m->reg[1] & 0x20);
    if (line && !m->int_line) {
        m->cpu.nmi = 1;
    }
    m->int_line = line;
}

static void vdp_ctrl(cv_t *m, uint8_t v) {
    if (!m->latch_on) {
        m->latch = v;
        m->latch_on = 1;
        m->addr = (m->addr & 0x3F00) | v;
        return;
    }
    m->latch_on = 0;
    if (v & 0x80) {
        m->reg[v & 7] = m->latch;
        if ((v & 7) == 1) {
            vdp_update_int(m);
        }
    } else {
        m->addr = ((v & 0x3F) << 8) | m->latch;
        if (!(v & 0x40)) {
            m->rdbuf = m->vram[m->addr];
            m->addr = (m->addr + 1) & 0x3FFF;
        }
    }
}

static uint8_t vdp_status(cv_t *m) {
    uint8_t s = m->status;
    m->status &= 0x1F;
    m->latch_on = 0;
    vdp_update_int(m);
    return s;
}

static inline void px8(uint8_t *d, uint8_t pat, uint8_t fg, uint8_t bg) {
    for (int b = 0; b < 8; b++) {
        d[b] = (pat & (0x80 >> b)) ? fg : bg;
    }
}

static void vdp_sprites(cv_t *m, int y) {
    uint8_t r1 = m->reg[1];
    int size = (r1 & 2) ? 16 : 8;
    int mag = r1 & 1;
    int h = size << mag;
    const uint8_t *sat = m->vram + ((m->reg[5] & 0x7F) << 7);
    const uint8_t *spg = m->vram + ((m->reg[6] & 7) << 11);
    uint8_t *line = m->line, *occ = m->occ;
    int count = 0, i;
    for (i = 0; i < 32; i++, sat += 4) {
        int sy = sat[0];
        if (sy == 0xD0) {
            break;
        }
        if (sy > 0xE0) {
            sy -= 256;
        }
        int row = y - (sy + 1);
        if (row < 0 || row >= h) {
            continue;
        }
        if (++count > 4) {
            if (!(m->status & 0x40)) {
                m->status = (m->status & 0xA0) | 0x40 | i;
            }
            return;
        }
        if (count == 1) {
            memset(occ, 0, CV_W);
        }
        row >>= mag;
        int name = sat[2];
        if (size == 16) {
            name &= 0xFC;
        }
        int x = sat[1];
        uint8_t col = sat[3];
        if (col & 0x80) {
            x -= 32;
        }
        col &= 15;
        const uint8_t *p = spg + name * 8 + row;
        uint32_t bits = p[0] << 8;
        if (size == 16) {
            bits |= p[16];
        }
        int w = size << mag;
        for (int px = 0; px < w; px++) {
            if (bits & (0x8000 >> (px >> mag))) {
                int sx = x + px;
                if ((unsigned)sx >= CV_W) {
                    continue;
                }
                if (occ[sx]) {
                    m->status |= 0x20;
                } else {
                    occ[sx] = 1;
                    if (col) {
                        line[sx] = col;
                    }
                }
            }
        }
    }
    if (!(m->status & 0x40)) {
        m->status = (m->status & 0xE0) | (i < 32 ? i : 31);
    }
}

// Compose line y (0-191) into m->line as colors 0-15, 0 meaning backdrop.
static void vdp_compose(cv_t *m, int y) {
    uint8_t r0 = m->reg[0], r1 = m->reg[1];
    uint8_t *line = m->line;
    const uint8_t *vram = m->vram;
    if (!(r1 & 0x40)) {
        memset(line, 0, CV_W);
        return;
    }
    const uint8_t *nt = vram + ((m->reg[2] & 0x0F) << 10);
    if (r1 & 0x10) {  // Text: 40 x 6 px, no sprites
        const uint8_t *pg = vram + ((m->reg[4] & 7) << 11) + (y & 7);
        uint8_t fg = m->reg[7] >> 4;
        nt += (y >> 3) * 40;
        memset(line, 0, 8);
        memset(line + 248, 0, 8);
        uint8_t *d = line + 8;
        for (int c = 0; c < 40; c++, d += 6) {
            uint8_t pat = pg[nt[c] * 8];
            for (int b = 0; b < 6; b++) {
                d[b] = (pat & (0x80 >> b)) ? fg : 0;
            }
        }
        return;
    }
    nt += (y >> 3) * 32;
    if (r1 & 0x08) {  // Multicolor: 4x4 blocks
        const uint8_t *pg = vram + ((m->reg[4] & 7) << 11) + ((y >> 3) & 3) * 2 + ((y >> 2) & 1);
        for (int c = 0; c < 32; c++) {
            uint8_t b = pg[nt[c] * 8];
            uint8_t *d = line + c * 8;
            d[0] = d[1] = d[2] = d[3] = b >> 4;
            d[4] = d[5] = d[6] = d[7] = b & 15;
        }
    } else if (r0 & 0x02) {  // Graphics II: three pattern/color banks
        uint32_t cmask = ((m->reg[3] & 0x7F) << 3) | 7;
        uint32_t pmask = ((m->reg[4] & 3) << 8) | (cmask & 0xFF);
        const uint8_t *pg = vram + ((m->reg[4] & 4) << 11) + (y & 7);
        const uint8_t *ct = vram + ((m->reg[3] & 0x80) << 6) + (y & 7);
        uint32_t bank = (y & 0xC0) << 2;
        for (int c = 0; c < 32; c++) {
            uint32_t ch = bank | nt[c];
            uint8_t col = ct[(ch & cmask) * 8];
            px8(line + c * 8, pg[(ch & pmask) * 8], col >> 4, col & 15);
        }
    } else {  // Graphics I
        const uint8_t *pg = vram + ((m->reg[4] & 7) << 11) + (y & 7);
        const uint8_t *ct = vram + (m->reg[3] << 6);
        for (int c = 0; c < 32; c++) {
            uint8_t n = nt[c];
            uint8_t col = ct[n >> 3];
            px8(line + c * 8, pg[n * 8], col >> 4, col & 15);
        }
    }
    vdp_sprites(m, y);
}

static const uint32_t cv_crc_nibble[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

static void cv_fill(cv_t *m, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c) {
    for (uint32_t j = 0; j < h; j++) {
        uint32_t off = (y + j) * m->fb_width + x;
        if (m->bpp == 8) {
            memset(m->fb + off, m->lut8[c], w);
        } else {
            uint16_t *d = (uint16_t *)m->fb + off;
            uint16_t v = m->lut16[c];
            for (uint32_t i = 0; i < w; i++) {
                d[i] = v;
            }
        }
    }
}

static void vdp_output(cv_t *m, int y) {
    uint8_t bd = m->reg[7] & 15;
    const uint8_t *line = m->line;
    if (m->crc_on) {
        uint32_t c = y ? m->crc : 0xFFFFFFFF;
        for (int x = 0; x < CV_W; x++) {
            c ^= line[x] ? line[x] : bd;
            c = (c >> 4) ^ cv_crc_nibble[c & 15];
            c = (c >> 4) ^ cv_crc_nibble[c & 15];
        }
        m->crc = c;
        if (y == CV_H - 1) {
            m->last_crc = ~c;
        }
    }
    if (m->fb == NULL) {
        return;
    }
    m->lut16[0] = cv_pal565[bd];
    m->lut8[0] = cv_pal332[bd];
    if (y == 0 && m->border != bd) {
        // Border outside the picture in the backdrop color, redrawn on change.
        uint32_t fw = m->fb_width, fh = m->fb_height;
        cv_fill(m, 0, 0, fw, m->y0, 0);
        cv_fill(m, 0, m->y0 + CV_H, fw, fh - m->y0 - CV_H, 0);
        cv_fill(m, 0, m->y0, m->x0, CV_H, 0);
        cv_fill(m, m->x0 + CV_W, m->y0, fw - m->x0 - CV_W, CV_H, 0);
        m->border = bd;
    }
    uint32_t off = (m->y0 + y) * m->fb_width + m->x0;
    if (m->bpp == 8) {
        const uint8_t *lut = m->lut8;
        uint8_t *d = m->fb + off;
        for (int x = 0; x < CV_W; x++) {
            d[x] = lut[line[x]];
        }
    } else {
        const uint16_t *lut = m->lut16;
        uint16_t *d = (uint16_t *)m->fb + off;
        for (int x = 0; x < CV_W; x++) {
            d[x] = lut[line[x]];
        }
    }
}

// ---- PSG ---------------------------------------------------------------------

static void psg_render(cv_t *m, uint32_t to) {
    while (m->audio_pos < to) {
        int32_t s = 0;
        for (int ch = 0; ch < 3; ch++) {
            uint32_t per = m->period[ch] ? m->period[ch] : 0x400;
            int32_t v = cv_vol[m->vol[ch]];
            if (per < 6) {  // above hearing: a level, used for sample playback
                s += v;
                continue;
            }
            m->cnt[ch] -= CV_PSG_STEP;
            while (m->cnt[ch] <= 0) {
                m->cnt[ch] += per << 16;
                m->out[ch] ^= 1;
            }
            s += m->out[ch] ? v : -v;
        }
        uint32_t np = m->noise_ctl & 3;
        np = np == 3 ? (m->period[2] ? m->period[2] : 0x400) : (0x10u << np);
        m->cnt[3] -= CV_PSG_STEP;
        while (m->cnt[3] <= 0) {
            m->cnt[3] += np << 16;
            m->out[3] ^= 1;
            if (m->out[3]) {
                uint16_t l = m->lfsr;
                uint16_t fb = (m->noise_ctl & 4) ? ((l ^ (l >> 1)) & 1) : (l & 1);
                m->lfsr = (l >> 1) | (fb << 14);
            }
        }
        int32_t nv = cv_vol[m->vol[3]];
        s += (m->lfsr & 1) ? nv : -nv;
        m->audio[m->audio_pos++] = s;
    }
}

static void psg_write(cv_t *m, uint8_t v) {
    uint32_t t = (uint32_t)m->cpu.cyc * CV_AUDIO_SAMPLES / CV_FRAME_CYC;
    psg_render(m, t < CV_AUDIO_SAMPLES ? t : CV_AUDIO_SAMPLES);
    if (v & 0x80) {
        m->psg_latch = (v >> 4) & 7;
    }
    int ch = m->psg_latch >> 1;
    if (m->psg_latch & 1) {
        m->vol[ch] = v & 15;
    } else if (ch == 3) {
        m->noise_ctl = v & 7;
        m->lfsr = 0x4000;
    } else if (v & 0x80) {
        m->period[ch] = (m->period[ch] & 0x3F0) | (v & 15);
    } else {
        m->period[ch] = (m->period[ch] & 15) | ((v & 0x3F) << 4);
    }
}

// ---- I/O ---------------------------------------------------------------------

static uint8_t cv_pad(cv_t *m, int p) {
    uint32_t in = m->input[p];
    uint8_t v = 0x7F;
    if (m->joy_mode) {
        v &= ~(in & 0x0F);
        if (in & CV_FIRE_L) {
            v &= ~0x40;
        }
        return v;
    }
    if (in & CV_FIRE_R) {
        v &= ~0x40;
    }
    uint32_t keys = (in >> 8) & 0xFFF;
    if (keys) {
        int k = 0;
        while (!(keys & 1)) {
            keys >>= 1;
            k++;
        }
        v = (v & 0xF0) | cv_keycode[k];
    }
    return v;
}

static uint8_t cv_in(cv_t *m, uint16_t port) {
    switch (port & 0xE0) {
        case 0xA0:
            if (port & 1) {
                return vdp_status(m);
            } else {
                uint8_t v = m->rdbuf;
                m->rdbuf = m->vram[m->addr];
                m->addr = (m->addr + 1) & 0x3FFF;
                m->latch_on = 0;
                return v;
            }
        case 0xE0:
            return cv_pad(m, (port >> 1) & 1);
    }
    return 0xFF;
}

static void cv_out(cv_t *m, uint16_t port, uint8_t v) {
    switch (port & 0xE0) {
        case 0x80:
            m->joy_mode = 0;
            break;
        case 0xC0:
            m->joy_mode = 1;
            break;
        case 0xA0:
            if (port & 1) {
                vdp_ctrl(m, v);
            } else {
                m->vram[m->addr] = v;
                m->rdbuf = v;
                m->addr = (m->addr + 1) & 0x3FFF;
                m->latch_on = 0;
            }
            break;
        case 0xE0:
            psg_write(m, v);
            break;
    }
}

// ---- machine -----------------------------------------------------------------

// rom must stay alive and be a multiple of 8 KB (16 KB above 32 KB); pad with
// 0xFF. bios is 8 KB or NULL for the built-in open BIOS.
// Returns 0, or 1 for a bad size.
static int cv_init(cv_t *m, const uint8_t *rom, uint32_t len, const uint8_t *bios) {
    memset(m, 0, sizeof(*m));
    if (len == 0 || (len & 0x1FFF) || len > (1u << 20) || (len > 0x8000 && (len & 0x3FFF))) {
        return 1;
    }
    m->bios = bios ? bios : cv_open_bios;
    m->rom = rom;
    m->rom_size = len;
    if (len > 0x8000) {
        m->banks = len >> 14;
        m->page[0] = rom + ((m->banks - 1) << 14);
        m->page[1] = m->page[0] + 0x2000;
        cv_bank(m, 0xFFC0);
    } else {
        for (int i = 0; i < 4; i++) {
            m->page[i] = rom + ((i << 13) % len);
        }
    }
    z80_reset(&m->cpu);
    for (int i = 0; i < 4; i++) {
        m->vol[i] = 15;
    }
    m->lfsr = 0x4000;
    for (int i = 0; i < 16; i++) {
        m->lut16[i] = cv_pal565[i];
        m->lut8[i] = cv_pal332[i];
    }
    m->border = 0xFF;
    return 0;
}

// 1 if the cartridge has a boot header (AA 55 or 55 AA at $8000).
static int cv_header_ok(cv_t *m) {
    uint8_t a = m->page[0][0], b = m->page[0][1];
    return (a == 0xAA && b == 0x55) || (a == 0x55 && b == 0xAA);
}

static void cv_run_frame(cv_t *m) {
    z80_t *z = &m->cpu;
    for (int y = 0; y < CV_LINES; y++) {
        if (y < CV_H) {
            vdp_compose(m, y);
            vdp_output(m, y);
        } else if (y == CV_H) {
            m->status |= 0x80;
            vdp_update_int(m);
        }
        z80_run(z, (y + 1) * CV_LINE_CYC);
    }
    z->cyc -= CV_FRAME_CYC;
    psg_render(m, CV_AUDIO_SAMPLES);
    m->audio_pos = 0;
    m->frame++;
    if (m->ring != NULL) {
        // Mono samples duplicated to stereo, copied into the ring with wrap.
        int16_t *ring = m->ring;
        uint32_t pos = m->ring_pos;
        for (uint32_t k = 0; k < CV_AUDIO_SAMPLES; k++) {
            ring[pos * 2] = m->audio[k];
            ring[pos * 2 + 1] = m->audio[k];
            if (++pos == m->ring_frames) {
                pos = 0;
            }
        }
        m->ring_pos = pos;
    }
}

#endif
