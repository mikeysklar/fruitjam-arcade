// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// MSX1 machine, written for this project: Z80 (z80.h), TMS9918A VDP drawn a
// scanline at a time (same code as ../coleco/src/cv.h), AY-3-8910 PSG, 8255
// PPI (slot select, keyboard matrix, key click), cartridge mappers, SCC.
//
// Slots, like the C-BIOS MSX1 machine in openMSX (none expanded):
//   0: BIOS ROM $0000-$7FFF (32 KB), C-BIOS logo ROM $8000-$BFFF
//   1: cartridge
//   2: empty
//   3: 64 KB RAM
// I/O (low address byte): $98/$99 VDP, $A0/$A1/$A2 PSG latch/write/read,
// $A8-$AB PPI. Anything else reads $FF.
//
// Timing: 228 CPU cycles per line, 262 lines, 59736 cycles (59.92 Hz) a
// frame, plus the MSX's one wait state per M1 cycle (taken from the R
// register count after each line). Each visible line is drawn at the start
// of its slice. The VDP frame flag is set at line 192; its interrupt output
// is the Z80 INT line (level).
#ifndef MSX_MACHINE_H
#define MSX_MACHINE_H

#include <stdint.h>
#include <string.h>

#define MSX_W 256
#define MSX_H 192
#define MSX_LINE_CYC 228
#define MSX_LINES 262
#define MSX_FRAME_CYC (MSX_LINE_CYC * MSX_LINES)
#define MSX_AUDIO_RATE 22050
#define MSX_AUDIO_SAMPLES 368  // 22050 / 59.92
#define MSX_PSG_STEP 664934  // (1789773 / 8 / 22050) in 16.16: AY ticks per sample
#define MSX_SCC_STEP 10638959u  // (3579545 / 22050) in 16.16: SCC clocks per sample

// Joystick bits, 1 = pressed. Same order as PSG register 14 (active low there).
#define MSX_UP 0x01
#define MSX_DOWN 0x02
#define MSX_LEFT 0x04
#define MSX_RIGHT 0x08
#define MSX_TRIG_A 0x10
#define MSX_TRIG_B 0x20

// Cartridge mappers.
#define MSX_MAP_PLAIN 0
#define MSX_MAP_KONAMI 1  // Konami without SCC: banks at $6000/$8000/$A000
#define MSX_MAP_KONAMI_SCC 2  // $5000/$7000/$9000/$B000, SCC at $9800
#define MSX_MAP_ASCII8 3  // $6000/$6800/$7000/$7800
#define MSX_MAP_ASCII16 4  // $6000/$7000
#define MSX_MAP_COUNT 5

#define MSX_KBD_ROWS 11

struct msx_s;
static inline uint8_t msx_rd(struct msx_s *m, uint16_t a);
static inline void msx_wr(struct msx_s *m, uint16_t a, uint8_t v);
static uint8_t msx_in(struct msx_s *m, uint16_t port);
static void msx_out(struct msx_s *m, uint16_t port, uint8_t v);

#define Z80_RD(a) msx_rd((struct msx_s *)z, (a))
#define Z80_WR(a, v) msx_wr((struct msx_s *)z, (a), (v))
#define Z80_IN(p) msx_in((struct msx_s *)z, (p))
#define Z80_OUT(p, v) msx_out((struct msx_s *)z, (p), (v))
#include "z80.h"
#include "cbios.h"

typedef struct msx_s {
    z80_t cpu;  // first, so z80_t * and msx_t * are the same pointer
    // Memory map: 8 KB regions as seen by the CPU now. NULL = slow path
    // (unmapped reads, SCC registers, mapper writes, ROM writes).
    const uint8_t *rd[8];
    uint8_t *wr[8];
    const uint8_t *slot_rd[4][8];  // what each primary slot shows in each region
    const uint8_t *bios;
    const uint8_t *logo;
    const uint8_t *rom;
    uint32_t rom_size;
    uint32_t rom_banks;  // 8 KB banks
    uint32_t rom_base;  // plain ROMs: CPU address of the first byte
    uint32_t mapper;
    uint8_t bank[4];  // 8 KB bank numbers at $4000 $6000 $8000 $A000
    uint8_t scc_on;
    uint8_t slot_reg;  // PPI port A ($A8)
    uint8_t ppi_c;  // PPI port C ($AA): keyboard row, click
    uint8_t kbd[16];  // pressed keys, 1 = pressed (inverted on read)
    uint32_t joy[2];
    uint32_t frame;
    // VDP
    uint8_t reg[8];
    uint8_t status;
    uint8_t latch_on;
    uint8_t latch;
    uint8_t rdbuf;
    uint16_t addr;
    // PSG (AY-3-8910)
    uint8_t psg_addr;
    uint8_t psg[16];
    uint8_t tone_out[3];
    uint8_t noise_out;
    int32_t tone_cnt[3];
    int32_t noise_cnt;
    int32_t env_cnt;
    uint32_t lfsr;
    int8_t env_pos;
    uint8_t env_attack;
    uint8_t env_hold;
    // SCC
    int8_t scc_wave[5][32];
    uint16_t scc_per[5];
    uint8_t scc_vol[5];
    uint8_t scc_en;
    uint32_t scc_phase[5];
    uint32_t scc_inc[5];
    // Mixer
    int32_t dc_x;
    int32_t dc_y;
    uint32_t audio_pos;
    int16_t audio[MSX_AUDIO_SAMPLES];
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
    uint8_t line[MSX_W];
    uint8_t occ[MSX_W];
    uint8_t ram[65536];
    uint8_t vram[16384];
} msx_t;

// TMS9918A colors (commonly used measured values); 0 is transparent.
static const uint16_t msx_pal565[16] = {
    0x0000, 0x0000, 0x2648, 0x5EEF, 0x52BD, 0x7BBF, 0xD289, 0x475E,
    0xFAAA, 0xFBCF, 0xD60A, 0xE670, 0x2587, 0xCAD7, 0xCE79, 0xFFFF,
};
static const uint8_t msx_pal332[16] = {
    0x00, 0x00, 0x39, 0x59, 0x4B, 0x6F, 0xC9, 0x5F, 0xE9, 0xED, 0xD9, 0xFA, 0x34, 0xCA, 0xDB, 0xFF,
};
// AY-3-8910 output levels (about 3 dB a step, 0 is silent), one channel's peak.
static const int16_t ay_vol[16] = {
    0, 66, 98, 140, 203, 297, 407, 657, 812, 1271, 1693, 2160, 2738, 3299, 4071, 4800,
};

// ---- memory --------------------------------------------------------------------

static void msx_update_map(msx_t *m) {
    for (int i = 0; i < 8; i++) {
        int slot = (m->slot_reg >> ((i >> 1) * 2)) & 3;
        m->rd[i] = m->slot_rd[slot][i];
        m->wr[i] = slot == 3 ? m->ram + (i << 13) : NULL;
    }
    // SCC registers sit over $9800-$9FFF of the cartridge while enabled.
    if (m->scc_on && ((m->slot_reg >> 4) & 3) == 1) {
        m->rd[4] = NULL;
    }
}

// Cartridge slot pages from the mapper's bank registers.
static void msx_cart_map(msx_t *m) {
    const uint8_t **p = m->slot_rd[1];
    if (m->mapper == MSX_MAP_PLAIN) {
        // Up to 16 KB is mirrored over a 16 KB window, bigger ROMs sit as is.
        uint32_t span = m->rom_size <= 0x4000 ? 0x4000 : m->rom_size;
        for (uint32_t i = 0; i < 8; i++) {
            int32_t off = (int32_t)(i << 13) - (int32_t)m->rom_base;
            p[i] = (off >= 0 && (uint32_t)off < span) ? m->rom + (off % m->rom_size) : NULL;
        }
    } else {
        p[0] = p[1] = p[6] = p[7] = NULL;
        for (int i = 0; i < 4; i++) {
            p[2 + i] = m->rom + ((m->bank[i] % m->rom_banks) << 13);
        }
    }
    msx_update_map(m);
}

static void scc_write(msx_t *m, uint8_t off, uint8_t v);
static void msx_audio_sync(msx_t *m);

static uint8_t msx_rd_slow(msx_t *m, uint16_t a) {
    int slot = (m->slot_reg >> ((a >> 14) * 2)) & 3;
    if (slot == 1 && m->scc_on && (a & 0xF800) == 0x9800) {
        uint8_t off = a & 0xFF;
        if (off < 0x80) {
            return (uint8_t)m->scc_wave[off >> 5][off & 31];
        }
        return 0xFF;
    }
    const uint8_t *p = m->slot_rd[slot][a >> 13];
    return p ? p[a & 0x1FFF] : 0xFF;
}

static inline uint8_t msx_rd(msx_t *m, uint16_t a) {
    const uint8_t *p = m->rd[a >> 13];
    if (p) {
        return p[a & 0x1FFF];
    }
    return msx_rd_slow(m, a);
}

static void msx_mapper_write(msx_t *m, uint16_t a, uint8_t v) {
    if (a < 0x4000 || a >= 0xC000) {
        return;
    }
    switch (m->mapper) {
        case MSX_MAP_KONAMI:
            if (a >= 0x6000) {
                m->bank[(a >> 13) - 2] = v;
                msx_cart_map(m);
            }
            break;
        case MSX_MAP_KONAMI_SCC:
            if ((a & 0x1800) == 0x1000) {
                int r = (a >> 13) - 2;
                m->bank[r] = v;
                if (r == 2) {
                    m->scc_on = (v & 0x3F) == 0x3F;
                }
                msx_cart_map(m);
            } else if (m->scc_on && (a & 0xF800) == 0x9800) {
                scc_write(m, a & 0xFF, v);
            }
            break;
        case MSX_MAP_ASCII8:
            if ((a & 0xE000) == 0x6000) {
                m->bank[(a >> 11) & 3] = v;
                msx_cart_map(m);
            }
            break;
        case MSX_MAP_ASCII16:
            if ((a & 0xF800) == 0x6000 || (a & 0xF800) == 0x7000) {
                int r = (a & 0x1000) ? 2 : 0;
                m->bank[r] = v * 2;
                m->bank[r + 1] = v * 2 + 1;
                msx_cart_map(m);
            }
            break;
    }
}

static inline void msx_wr(msx_t *m, uint16_t a, uint8_t v) {
    uint8_t *p = m->wr[a >> 13];
    if (p) {
        p[a & 0x1FFF] = v;
    } else if (((m->slot_reg >> ((a >> 14) * 2)) & 3) == 1) {
        msx_mapper_write(m, a, v);
    }
}

// ---- VDP -----------------------------------------------------------------------

static void vdp_update_int(msx_t *m) {
    m->cpu.irq = (m->status & 0x80) && (m->reg[1] & 0x20);
}

static void vdp_ctrl(msx_t *m, uint8_t v) {
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

static uint8_t vdp_status(msx_t *m) {
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

static void vdp_sprites(msx_t *m, int y) {
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
            memset(occ, 0, MSX_W);
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
                if ((unsigned)sx >= MSX_W) {
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
static void vdp_compose(msx_t *m, int y) {
    uint8_t r0 = m->reg[0], r1 = m->reg[1];
    uint8_t *line = m->line;
    const uint8_t *vram = m->vram;
    if (!(r1 & 0x40)) {
        memset(line, 0, MSX_W);
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

static const uint32_t msx_crc_nibble[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

static void msx_fill(msx_t *m, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c) {
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

static void vdp_output(msx_t *m, int y) {
    uint8_t bd = m->reg[7] & 15;
    const uint8_t *line = m->line;
    if (m->crc_on) {
        uint32_t c = y ? m->crc : 0xFFFFFFFF;
        for (int x = 0; x < MSX_W; x++) {
            c ^= line[x] ? line[x] : bd;
            c = (c >> 4) ^ msx_crc_nibble[c & 15];
            c = (c >> 4) ^ msx_crc_nibble[c & 15];
        }
        m->crc = c;
        if (y == MSX_H - 1) {
            m->last_crc = ~c;
        }
    }
    if (m->fb == NULL) {
        return;
    }
    m->lut16[0] = msx_pal565[bd];
    m->lut8[0] = msx_pal332[bd];
    if (y == 0 && m->border != bd) {
        // Border outside the picture in the backdrop color, redrawn on change.
        uint32_t fw = m->fb_width, fh = m->fb_height;
        msx_fill(m, 0, 0, fw, m->y0, 0);
        msx_fill(m, 0, m->y0 + MSX_H, fw, fh - m->y0 - MSX_H, 0);
        msx_fill(m, 0, m->y0, m->x0, MSX_H, 0);
        msx_fill(m, m->x0 + MSX_W, m->y0, fw - m->x0 - MSX_W, MSX_H, 0);
        m->border = bd;
    }
    uint32_t off = (m->y0 + y) * m->fb_width + m->x0;
    if (m->bpp == 8) {
        const uint8_t *lut = m->lut8;
        uint8_t *d = m->fb + off;
        for (int x = 0; x < MSX_W; x++) {
            d[x] = lut[line[x]];
        }
    } else {
        const uint16_t *lut = m->lut16;
        uint16_t *d = (uint16_t *)m->fb + off;
        for (int x = 0; x < MSX_W; x++) {
            d[x] = lut[line[x]];
        }
    }
}

// ---- sound: AY-3-8910, SCC, key click --------------------------------------------

static void env_step(msx_t *m) {
    if (m->env_hold) {
        return;
    }
    if (--m->env_pos >= 0) {
        return;
    }
    uint8_t shape = m->psg[13];
    if (!(shape & 8)) {  // one cycle, then silent
        m->env_attack = 0;
        m->env_pos = 0;
        m->env_hold = 1;
    } else if (shape & 1) {  // hold at the end (flipped if alternating)
        if (shape & 2) {
            m->env_attack ^= 15;
        }
        m->env_pos = 0;
        m->env_hold = 1;
    } else {
        if (shape & 2) {
            m->env_attack ^= 15;
        }
        m->env_pos = 15;
    }
}

static void audio_render(msx_t *m, uint32_t to) {
    const uint8_t *r = m->psg;
    while (m->audio_pos < to) {
        int32_t s = 0;
        // Noise: 17-bit LFSR, clocked at half the tone rate.
        uint32_t np = r[6] & 31;
        m->noise_cnt -= MSX_PSG_STEP;
        while (m->noise_cnt <= 0) {
            m->noise_cnt += (np ? np : 1) << 17;
            uint32_t l = m->lfsr;
            l = (l >> 1) | (((l ^ (l >> 3)) & 1) << 16);
            m->lfsr = l;
            m->noise_out = l & 1;
        }
        uint32_t ep = r[11] | (r[12] << 8);
        // Envelope steps every 2 x period ticks; counted in 24.8 to fit 16-bit periods.
        m->env_cnt -= MSX_PSG_STEP >> 9;
        while (m->env_cnt <= 0) {
            m->env_cnt += (ep ? ep : 1) << 8;
            env_step(m);
        }
        uint8_t mix = r[7];
        for (int ch = 0; ch < 3; ch++) {
            uint32_t per = r[ch * 2] | ((r[ch * 2 + 1] & 15) << 8);
            uint32_t tone = 1;
            if (per >= 5) {  // lower periods are above hearing: a steady level
                m->tone_cnt[ch] -= MSX_PSG_STEP;
                while (m->tone_cnt[ch] <= 0) {
                    m->tone_cnt[ch] += per << 16;
                    m->tone_out[ch] ^= 1;
                }
                tone = m->tone_out[ch];
            }
            tone |= (mix >> ch) & 1;
            tone &= m->noise_out | ((mix >> (ch + 3)) & 1);
            if (tone) {
                uint8_t v = r[8 + ch];
                s += ay_vol[(v & 0x10) ? (m->env_pos ^ m->env_attack) : (v & 15)];
            }
        }
        if (m->scc_en) {
            int32_t scc = 0;
            for (int ch = 0; ch < 5; ch++) {
                if ((m->scc_en >> ch) & 1) {
                    m->scc_phase[ch] += m->scc_inc[ch];
                    // Channel 5 plays channel 4's waveform on the plain SCC.
                    scc += m->scc_wave[ch < 4 ? ch : 3][(m->scc_phase[ch] >> 16) & 31] * m->scc_vol[ch];
                }
            }
            s += scc;  // 5 x 128 x 15 = 9600 peak
        }
        if (m->ppi_c & 0x80) {
            s += 2400;  // key click
        }
        // DC blocker: the AY output is one-sided.
        int32_t y = s - m->dc_x + ((m->dc_y * 32604) >> 15);
        m->dc_x = s;
        m->dc_y = y;
        if (y > 32767) {
            y = 32767;
        } else if (y < -32768) {
            y = -32768;
        }
        m->audio[m->audio_pos++] = y;
    }
}

static void msx_audio_sync(msx_t *m) {
    uint32_t t = (uint32_t)m->cpu.cyc * MSX_AUDIO_SAMPLES / MSX_FRAME_CYC;
    audio_render(m, t < MSX_AUDIO_SAMPLES ? t : MSX_AUDIO_SAMPLES);
}

static const uint8_t psg_mask[16] = {
    0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF, 0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF,
};

static void psg_write(msx_t *m, uint8_t v) {
    uint8_t a = m->psg_addr;
    if (a >= 16) {
        return;
    }
    if (a < 14) {
        msx_audio_sync(m);
    }
    m->psg[a] = v & psg_mask[a];
    if (a == 13) {
        m->env_attack = (v & 4) ? 15 : 0;
        m->env_pos = 15;
        m->env_hold = 0;
        uint32_t ep = m->psg[11] | (m->psg[12] << 8);
        m->env_cnt = (ep ? ep : 1) << 8;
    }
}

static uint8_t psg_read(msx_t *m) {
    uint8_t a = m->psg_addr;
    if (a == 14) {
        // Joystick port picked by register 15 bit 6. Bit 6 is the keyboard
        // layout (1 = JIS), bit 7 the cassette input.
        return 0x7F & ~(m->joy[(m->psg[15] >> 6) & 1] & 0x3F);
    }
    return a < 16 ? m->psg[a] : 0xFF;
}

static void scc_write(msx_t *m, uint8_t off, uint8_t v) {
    msx_audio_sync(m);
    if (off < 0x80) {
        m->scc_wave[off >> 5][off & 31] = (int8_t)v;
        return;
    }
    if (off >= 0xA0) {
        return;  // deformation register ($E0-$FF) not emulated
    }
    off &= 0x8F;  // $90-$9F mirror $80-$8F
    if (off < 0x8A) {
        int ch = (off - 0x80) >> 1;
        uint16_t p = m->scc_per[ch];
        p = (off & 1) ? (p & 0xFF) | ((v & 15) << 8) : (p & 0xF00) | v;
        m->scc_per[ch] = p;
        // Below 9 the real chip stops the channel.
        m->scc_inc[ch] = p < 9 ? 0 : MSX_SCC_STEP / (p + 1);
    } else if (off < 0x8F) {
        m->scc_vol[off - 0x8A] = v & 15;
    } else {
        m->scc_en = v & 31;
    }
}

// ---- I/O -----------------------------------------------------------------------

static uint8_t msx_in(msx_t *m, uint16_t port) {
    switch (port & 0xFF) {
        case 0x98: {
            uint8_t v = m->rdbuf;
            m->rdbuf = m->vram[m->addr];
            m->addr = (m->addr + 1) & 0x3FFF;
            m->latch_on = 0;
            return v;
        }
        case 0x99:
            return vdp_status(m);
        case 0xA2:
            return psg_read(m);
        case 0xA8:
            return m->slot_reg;
        case 0xA9: {
            uint32_t row = m->ppi_c & 15;
            return row < MSX_KBD_ROWS ? ~m->kbd[row] : 0xFF;
        }
        case 0xAA:
            return m->ppi_c;
    }
    return 0xFF;
}

static void ppi_set_c(msx_t *m, uint8_t v) {
    if ((v ^ m->ppi_c) & 0x80) {
        msx_audio_sync(m);
    }
    m->ppi_c = v;
}

static void msx_out(msx_t *m, uint16_t port, uint8_t v) {
    switch (port & 0xFF) {
        case 0x98:
            m->vram[m->addr] = v;
            m->rdbuf = v;
            m->addr = (m->addr + 1) & 0x3FFF;
            m->latch_on = 0;
            break;
        case 0x99:
            vdp_ctrl(m, v);
            break;
        case 0xA0:
            m->psg_addr = v;
            break;
        case 0xA1:
            psg_write(m, v);
            break;
        case 0xA8:
            m->slot_reg = v;
            msx_update_map(m);
            break;
        case 0xAA:
            ppi_set_c(m, v);
            break;
        case 0xAB:
            if (v & 0x80) {
                ppi_set_c(m, 0);  // mode set clears the outputs
            } else {
                uint8_t bit = 1 << ((v >> 1) & 7);
                ppi_set_c(m, (v & 1) ? (m->ppi_c | bit) : (m->ppi_c & ~bit));
            }
            break;
    }
}

// ---- cartridges ------------------------------------------------------------------

// Guess the mapper of a ROM over 64 KB from the addresses of its LD (nnnn),A
// instructions, the way openMSX and blueMSX do. Up to 64 KB is plain.
static uint32_t msx_guess_mapper(const uint8_t *rom, uint32_t len) {
    if (len <= 0x10000) {
        return MSX_MAP_PLAIN;
    }
    uint32_t n[MSX_MAP_COUNT] = {0};
    for (uint32_t i = 0; i + 3 <= len; i++) {
        if (rom[i] != 0x32) {
            continue;
        }
        switch (rom[i + 1] | (rom[i + 2] << 8)) {
            case 0x5000:
            case 0x9000:
            case 0xB000:
                n[MSX_MAP_KONAMI_SCC]++;
                break;
            case 0x4000:
            case 0x8000:
            case 0xA000:
                n[MSX_MAP_KONAMI]++;
                break;
            case 0x6800:
            case 0x7800:
                n[MSX_MAP_ASCII8]++;
                break;
            case 0x6000:
                n[MSX_MAP_KONAMI]++;
                n[MSX_MAP_ASCII8]++;
                n[MSX_MAP_ASCII16]++;
                break;
            case 0x7000:
                n[MSX_MAP_KONAMI_SCC]++;
                n[MSX_MAP_ASCII8]++;
                n[MSX_MAP_ASCII16]++;
                break;
            case 0x77FF:
                n[MSX_MAP_ASCII16]++;
                break;
        }
    }
    if (n[MSX_MAP_ASCII8]) {
        n[MSX_MAP_ASCII8]--;  // ties go to the others
    }
    // Ties: ASCII16 over Konami SCC over Konami over ASCII8; nothing found: ASCII8.
    static const uint8_t order[4] = {MSX_MAP_ASCII8, MSX_MAP_KONAMI, MSX_MAP_KONAMI_SCC, MSX_MAP_ASCII16};
    uint32_t best = MSX_MAP_ASCII8;
    for (int i = 0; i < 4; i++) {
        if (n[order[i]] && n[order[i]] >= n[best]) {
            best = order[i];
        }
    }
    return best;
}

// Where a plain ROM goes: from its "AB" header and start address.
static uint32_t msx_plain_base(const uint8_t *rom, uint32_t len) {
    int hdr0 = rom[0] == 'A' && rom[1] == 'B';
    int hdr4 = len > 0x4000 && rom[0x4000] == 'A' && rom[0x4001] == 'B';
    if (len > 0xC000 || (hdr4 && !hdr0)) {
        return 0;  // 48/64 KB images that start at $0000
    }
    if (hdr0 && len <= 0x4000) {
        uint32_t init = rom[2] | (rom[3] << 8);
        uint32_t text = rom[8] | (rom[9] << 8);  // BASIC program ROMs
        uint32_t at = init ? init : text;
        if (at >= 0x8000 && at < 0xC000) {
            return 0x8000;
        }
    }
    return 0x4000;
}

// rom must stay alive and be a whole number of 8 KB banks (pad with 0xFF).
// bios is 32 KB or NULL for the built-in C-BIOS. mapper < 0 guesses.
// Returns 0, or 1 for a bad size, 2 for an unknown mapper.
static int msx_init(msx_t *m, const uint8_t *rom, uint32_t len, const uint8_t *bios, int mapper) {
    memset(m, 0, sizeof(*m));
    if (len == 0 || (len & 0x1FFF) || len > (8u << 20)) {
        return 1;
    }
    if (mapper < 0) {
        mapper = msx_guess_mapper(rom, len);
    }
    if (mapper >= MSX_MAP_COUNT || (mapper == MSX_MAP_PLAIN && len > 0x10000)) {
        return 2;
    }
    m->bios = bios ? bios : cbios_main;
    m->logo = bios ? NULL : cbios_logo;
    m->rom = rom;
    m->rom_size = len;
    m->rom_banks = len >> 13;
    m->mapper = mapper;
    if (mapper == MSX_MAP_PLAIN) {
        m->rom_base = msx_plain_base(rom, len);
    } else if (mapper == MSX_MAP_KONAMI || mapper == MSX_MAP_KONAMI_SCC) {
        for (int i = 0; i < 4; i++) {
            m->bank[i] = i;
        }
    }
    for (int i = 0; i < 4; i++) {
        m->slot_rd[0][i] = m->bios + (i << 13);
    }
    if (m->logo) {
        m->slot_rd[0][4] = m->logo;
        m->slot_rd[0][5] = m->logo + 0x2000;
    }
    for (int i = 0; i < 8; i++) {
        m->slot_rd[3][i] = m->ram + (i << 13);
    }
    msx_cart_map(m);  // also builds rd/wr for slot_reg 0
    z80_reset(&m->cpu);
    m->cpu.irq_vec = 0xFF;  // IM 0 would see RST 38h on the bus
    m->lfsr = 1;
    for (int i = 0; i < 16; i++) {
        m->lut16[i] = msx_pal565[i];
        m->lut8[i] = msx_pal332[i];
    }
    m->border = 0xFF;
    return 0;
}

static void msx_run_frame(msx_t *m) {
    z80_t *z = &m->cpu;
    for (int y = 0; y < MSX_LINES; y++) {
        if (y < MSX_H) {
            vdp_compose(m, y);
            vdp_output(m, y);
        } else if (y == MSX_H) {
            m->status |= 0x80;
            vdp_update_int(m);
        }
        uint8_t r0 = z->r;
        z80_run(z, (y + 1) * MSX_LINE_CYC);
        // One wait state per M1 cycle. LD R,A can upset the count; ignore that line.
        uint8_t m1 = z->r - r0;
        if (m1 < 64) {
            z->cyc += m1;
        }
    }
    z->cyc -= MSX_FRAME_CYC;
    audio_render(m, MSX_AUDIO_SAMPLES);
    m->audio_pos = 0;
    m->frame++;
    if (m->ring != NULL) {
        // Mono samples duplicated to stereo, copied into the ring with wrap.
        int16_t *ring = m->ring;
        uint32_t pos = m->ring_pos;
        for (uint32_t k = 0; k < MSX_AUDIO_SAMPLES; k++) {
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
