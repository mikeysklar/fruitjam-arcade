// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Watara Supervision core: 65C02 (cpu65c02.h), 8 KB WRAM, 8 KB VRAM, the
// 160x160 2 bpp LCD, VRAM DMA, IRQ timer, NMI, ROM banking, and the sound
// unit (two squares, noise, audio DMA).
//
// Written from Kevin Horton's "Supervision Reverse Engineering Notes" V4.00
// (2011, Unlicense, github.com/GrenderG/supervision_reveng_notes). Where the
// notes are silent, MAME's svision driver was read as a behaviour reference
// only (no code taken): the DMA IRQ enable on system control bit 2, the
// open-bus and register read-back behaviour. No BIOS exists or is needed.
//
// Timing: one CPU cycle is one tick (4 MHz). The NMI comes every 65536
// cycles (61.04 Hz) whatever the LCD does, so one emulated "frame" is one NMI
// period and the picture is taken from VRAM at that point. The real LCD scans
// at 50.8 Hz, unsynchronised; games cannot see it.
//
// Nothing here allocates: the caller owns sv_t (which holds all RAM), the
// ROM, the framebuffer and the audio ring.

#include <stdint.h>

#define SV_W 160
#define SV_H 160
#define SV_CLOCK 4000000u
#define SV_FRAME_TICKS 65536u
#define SV_AUDIO_RATE 22050
#define SV_SAMPLE_TICKS (SV_CLOCK / SV_AUDIO_RATE)  // 181
#define SV_SAMPLE_FRAC (SV_CLOCK % SV_AUDIO_RATE)   // 8950
#define SV_LEVEL_SCALE 2000  // int16 per DAC step; the 4-bit DAC clips at 15

// Joypad bits: register $2020 layout, but 1 = pressed here.
#define SV_RIGHT 0x01
#define SV_LEFT 0x02
#define SV_DOWN 0x04
#define SV_UP 0x08
#define SV_B 0x10
#define SV_A 0x20
#define SV_SELECT 0x40
#define SV_START 0x80

// Output modes for the 320x240 (or any) framebuffer.
#define SV_MODE_SMOOTH 0  // 1.5x, 240x240, in-between pixels blended
#define SV_MODE_1X 1      // 160x160
#define SV_MODE_SHARP 2   // 1.5x, 240x240, nearest neighbour

#define SV_ERR_STP 3

typedef struct {
    uint8_t reg[4];    // freq low, freq high, enable/duty/volume, length
    uint32_t period;   // cycles, 32 * (F + 1): Fout = 125 kHz / (F + 1)
    uint32_t phase;    // 0 .. period-1
    uint16_t len;      // 65536-cycle counts left (L + 1 when written)
} sv_square_t;

typedef struct sv_s {
    // CPU
    uint8_t A, X, Y, S;
    uint16_t PC;
    unsigned C_, V_, D_, I_, N_, Z_;
    uint8_t wai, stopped, err, nmi_pending;
    uint8_t irq;  // IRQ line level

    // time
    uint32_t ticks;       // CPU cycles, wraps (all compares are signed differences)
    uint32_t next_frame;  // next NMI
    uint32_t next_event;
    uint32_t frames;

    // system
    uint8_t reg[0x40];
    uint8_t joy;
    uint8_t timer_on, timer_flag, dma_flag;
    uint32_t timer_end, timer_presc;

    // cartridge
    const uint8_t *rom;
    uint32_t rom_size, rom_pages, banks;
    // CPU read map in 8 KB pages: WRAM, NULL (registers), VRAM, NULL (open
    // bus), banked ROM, fixed ROM
    const uint8_t *page[8];

    // video
    uint8_t dirty, pal_dirty, crc_on, mode;
    uint8_t *fb;
    uint16_t fb_w, fb_h, x0, y0;
    uint8_t bpp;
    uint32_t pal[4];   // RGB888, shade 0 (lightest) .. 3
    uint16_t lut[13];  // colour for a weighted sum of four shades, 0..12
    uint32_t last_crc;

    // sound
    sv_square_t sq[2];
    uint32_t noise_div, noise_left;  // cycles per LFSR clock, cycles to the next
    uint16_t lfsr, noise_len;
    uint8_t adma_on, adma_byte, adma_nib;
    uint16_t adma_addr;
    uint32_t adma_nibbles, adma_left, adma_end;  // nibbles to go, cycles to the next
    uint32_t audio_t, next_sample, sample_frac;
    uint32_t acc_l, acc_r, acc_n;  // level * cycles in the sample being built
    int16_t *ring;
    uint32_t ring_frames, ring_pos, samples;

    uint8_t wram[0x2000];
    uint8_t vram[0x2000];
    uint8_t line[SV_H][SV_W / 4];  // the picture, 2 bpp, pixel 0 in bits 0-1
    uint8_t changed[SV_H];
} sv_t;

static const uint32_t sv_noise_div[16] = {
    8, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 65536, 131072,
};

// ---- CRC (frame checks in the host tests) ---------------------------------------
static const uint32_t sv_crc_nibble[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

static uint32_t sv_crc32(const uint8_t *p, uint32_t len) {
    uint32_t c = 0xFFFFFFFFu;
    while (len--) {
        c ^= *p++;
        c = (c >> 4) ^ sv_crc_nibble[c & 15];
        c = (c >> 4) ^ sv_crc_nibble[c & 15];
    }
    return ~c;
}

// ---- memory ------------------------------------------------------------------
static void sv_map(sv_t *s) {
    uint32_t b = ((s->reg[0x26] >> 5) & 7) % s->banks;
    s->page[4] = s->rom + ((2 * b) % s->rom_pages) * 0x2000;
    s->page[5] = s->rom + ((2 * b + 1) % s->rom_pages) * 0x2000;
    uint32_t last = s->rom_pages >= 2 ? s->rom_pages - 2 : 0;
    s->page[6] = s->rom + last * 0x2000;
    s->page[7] = s->rom + ((last + 1) % s->rom_pages) * 0x2000;
}

// Bus read for the DMA units (no side effects; registers read as open bus).
static uint8_t sv_peek(sv_t *s, uint16_t a) {
    if (a < 0x2000) {
        return s->wram[a];
    }
    if (a >= 0x8000) {
        return s->page[a >> 13][a & 0x1FFF];
    }
    if (a >= 0x4000 && a < 0x6000) {
        return s->vram[a - 0x4000];
    }
    return 0xFF;
}

// ---- events ------------------------------------------------------------------
static void sv_update_irq(sv_t *s) {
    s->irq = (s->timer_flag && (s->reg[0x26] & 2)) || (s->dma_flag && (s->reg[0x26] & 4));
}

static void sv_set_next_event(sv_t *s) {
    uint32_t t = s->next_frame;
    if (s->timer_on && (int32_t)(s->timer_end - t) < 0) {
        t = s->timer_end;
    }
    if (s->adma_on && (int32_t)(s->adma_end - t) < 0) {
        t = s->adma_end;
    }
    s->next_event = t;
}

// ---- sound -------------------------------------------------------------------
// High cycles of a square wave in [phase, phase + d), d < period.
static uint32_t sq_high_part(uint32_t phase, uint32_t d, uint32_t hi, uint32_t period) {
    uint32_t e = phase + d;
    uint32_t a = phase < hi ? hi - phase : 0;  // high left in this period from phase
    if (e <= period) {
        uint32_t ee = e < hi ? hi - e : 0;
        return a - ee;
    }
    e -= period;
    return a + (e < hi ? e : hi);
}

static void sv_audio_span(sv_t *s, uint32_t d) {
    uint32_t l = 0, r = 0;
    for (int k = 0; k < 2; k++) {
        sv_square_t *q = &s->sq[k];
        uint32_t vol = q->reg[2] & 15;
        uint32_t period = q->period;
        uint32_t ph = q->phase + d;
        if (vol && ((q->reg[2] & 0x40) || q->len)) {
            static const uint8_t duty_8ths[4] = {1, 2, 4, 6};  // 12.5, 25, 50, 75 %
            uint32_t hi = period * duty_8ths[(q->reg[2] >> 4) & 3] / 8;
            uint32_t n = d / period, rem = d - n * period;
            uint32_t high = n * hi + sq_high_part(q->phase, rem, hi, period);
            if (k == 0) {
                r += high * vol;
            } else {
                l += high * vol;
            }
        }
        q->phase = ph % period;
    }
    uint8_t nc = s->reg[0x2A];
    if ((nc & 0x10) && ((nc & 2) || s->noise_len)) {
        uint32_t vol = s->reg[0x28] & 15;
        uint32_t high = 0, left = d;
        uint16_t top = (nc & 1) ? 0x4000 : 0x40;
        while (left) {
            uint32_t run = left < s->noise_left ? left : s->noise_left;
            if (s->lfsr & top) {
                high += run;
            }
            left -= run;
            s->noise_left -= run;
            if (!s->noise_left) {
                unsigned fb = (nc & 1) ? ((s->lfsr >> 14) ^ (s->lfsr >> 13)) & 1
                                       : ((s->lfsr >> 6) ^ (s->lfsr >> 5)) & 1;
                s->lfsr = (uint16_t)(((s->lfsr << 1) | fb) & 0x7FFF);
                s->noise_left = s->noise_div;
            }
        }
        high *= vol;
        if (nc & 8) {
            l += high;
        }
        if (nc & 4) {
            r += high;
        }
    }
    if (s->adma_on) {
        uint32_t period = 256u << (s->reg[0x1B] & 3);
        uint32_t left = d, out = 0;
        while (left && s->adma_on) {
            uint32_t run = left < s->adma_left ? left : s->adma_left;
            out += run * (s->adma_nib ? s->adma_byte & 15 : s->adma_byte >> 4);
            left -= run;
            s->adma_left -= run;
            if (!s->adma_left) {
                s->adma_left = period;
                if (--s->adma_nibbles == 0) {
                    s->adma_on = 0;
                    s->dma_flag = 1;
                } else if (s->adma_nib) {
                    s->adma_nib = 0;
                    s->adma_addr++;
                    uint16_t a = s->adma_addr;
                    if (a >= 0x8000 && a < 0xC000) {  // own bank bits for $8000-$BFFF
                        uint32_t b = ((s->reg[0x1B] >> 4) & 7) % s->banks;
                        s->adma_byte = s->rom[((2 * b + ((a >> 13) & 1)) % s->rom_pages) * 0x2000 + (a & 0x1FFF)];
                    } else {
                        s->adma_byte = sv_peek(s, a);
                    }
                } else {
                    s->adma_nib = 1;
                }
            }
        }
        if (s->reg[0x1B] & 8) {
            l += out;
        }
        if (s->reg[0x1B] & 4) {
            r += out;
        }
    }
    s->acc_l += l;
    s->acc_r += r;
    s->acc_n += d;
}

static void sv_emit(sv_t *s) {
    uint32_t n = s->acc_n ? s->acc_n : 1;
    uint32_t l = s->acc_l * SV_LEVEL_SCALE / n, r = s->acc_r * SV_LEVEL_SCALE / n;
    if (l > 15 * SV_LEVEL_SCALE) {
        l = 15 * SV_LEVEL_SCALE;
    }
    if (r > 15 * SV_LEVEL_SCALE) {
        r = 15 * SV_LEVEL_SCALE;
    }
    s->acc_l = s->acc_r = s->acc_n = 0;
    if (s->ring) {
        s->ring[2 * s->ring_pos] = (int16_t)l;
        s->ring[2 * s->ring_pos + 1] = (int16_t)r;
        if (++s->ring_pos >= s->ring_frames) {
            s->ring_pos = 0;
        }
    }
    s->samples++;
}

// Bring the sound unit up to time t.
static void sv_audio_sync(sv_t *s, uint32_t t) {
    while ((int32_t)(t - s->audio_t) > 0) {
        uint32_t end = (int32_t)(s->next_sample - t) < 0 ? s->next_sample : t;
        sv_audio_span(s, end - s->audio_t);
        s->audio_t = end;
        if (end == s->next_sample) {
            sv_emit(s);
            s->next_sample += SV_SAMPLE_TICKS;
            s->sample_frac += SV_SAMPLE_FRAC;
            if (s->sample_frac >= SV_AUDIO_RATE) {
                s->sample_frac -= SV_AUDIO_RATE;
                s->next_sample++;
            }
        }
    }
}

static void sv_adma_schedule(sv_t *s) {
    uint32_t period = 256u << (s->reg[0x1B] & 3);
    s->adma_end = s->ticks + s->adma_left + (s->adma_nibbles - 1) * period;
}

// ---- video -------------------------------------------------------------------
static uint16_t sv_pixel(const sv_t *s, uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    if (s->bpp == 16) {
        return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
    return (uint16_t)((r & 0xE0) | ((g & 0xE0) >> 3) | (b >> 6));
}

static void sv_build_lut(sv_t *s) {
    for (int k = 0; k <= 12; k++) {
        int i = k / 4, f = k % 4;
        uint32_t a = s->pal[i], b = s->pal[i < 3 ? i + 1 : 3], rgb = 0;
        for (int sh = 0; sh < 24; sh += 8) {
            uint32_t ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
            rgb |= ((ca * (4 - f) + cb * f) / 4) << sh;
        }
        s->lut[k] = sv_pixel(s, rgb);
    }
}

// VRAM -> line[] (the picture as 2 bpp, 40 bytes a line) following the
// notes' scan pseudocode: lines are $30 bytes apart ($60 when LCD_X_Size >
// $C3), the pointer wraps at 8 KB, and a line start of $1FE0 wraps to 0 (170
// lines fill VRAM). Lines that differ from last time are flagged in changed[].
static void sv_render_lines(sv_t *s) {
    uint8_t *changed = s->changed;
    if (!(s->reg[0x26] & 8)) {
        for (uint32_t y = 0; y < SV_H; y++) {
            uint8_t *out = s->line[y];
            for (uint32_t g = 0; g < SV_W / 4; g++) {
                if (out[g]) {
                    out[g] = 0;
                    changed[y] = 1;
                }
            }
        }
        return;
    }
    uint32_t xs = s->reg[2], xsize = s->reg[0];
    uint32_t step = xsize > 0xC3 ? 0x60 : 0x30;
    uint32_t groups = (xsize & 0xFC) >> 2;
    if (groups > SV_W / 4) {
        groups = SV_W / 4;
    }
    uint32_t shift = (xs & 3) * 2;
    uint32_t vstart = (s->reg[3] * 0x30u) & 0x1FFF;
    if (vstart == 0x1FE0) {
        vstart = 0;
    }
    const uint8_t *vram = s->vram;
    for (uint32_t y = 0; y < SV_H; y++) {
        uint8_t *out = s->line[y];
        uint32_t a = vstart + (xs >> 2);
        uint32_t cur = vram[a & 0x1FFF], diff = 0;
        for (uint32_t g = 0; g < groups; g++) {
            uint32_t nxt = vram[(a + g + 1) & 0x1FFF];
            uint8_t p = (uint8_t)((cur | (nxt << 8)) >> shift);
            diff |= p ^ out[g];
            out[g] = p;
            cur = nxt;
        }
        if (diff) {
            changed[y] = 1;
        }
        vstart = (vstart + step) & 0x1FFF;
        if (vstart == 0x1FE0) {
            vstart = 0;
        }
    }
}

// Draw the flagged lines (all of them after a palette or mode change).
#define SV_BLIT(T) do { \
        T *fb = (T *)s->fb; \
        const uint16_t *lut = s->lut; \
        uint32_t w = s->fb_w; \
        if (s->mode == SV_MODE_1X) { \
            for (uint32_t y = 0; y < SV_H; y++) { \
                if (!all && !s->changed[y]) continue; \
                const uint8_t *src = s->line[y]; \
                T *d = fb + (s->y0 + y) * w + s->x0; \
                for (uint32_t g = 0; g < SV_W / 4; g++) { \
                    uint32_t p = src[g]; \
                    d[0] = (T)lut[(p & 3) * 4]; d[1] = (T)lut[((p >> 2) & 3) * 4]; \
                    d[2] = (T)lut[((p >> 4) & 3) * 4]; d[3] = (T)lut[(p >> 6) * 4]; \
                    d += 4; \
                } \
            } \
        } else { \
            for (uint32_t j = 0; j < SV_H / 2; j++) { \
                if (!all && !s->changed[2 * j] && !s->changed[2 * j + 1]) continue; \
                const uint8_t *r0 = s->line[2 * j], *r1 = s->line[2 * j + 1]; \
                T *d0 = fb + (s->y0 + 3 * j) * w + s->x0, *d1 = d0 + w, *d2 = d1 + w; \
                int smooth = s->mode == SV_MODE_SMOOTH; \
                for (uint32_t g = 0; g < SV_W / 4; g++) { \
                    uint32_t p0 = r0[g], p1 = r1[g]; \
                    for (int h = 0; h < 2; h++) { \
                        uint32_t a = p0 & 3, b = (p0 >> 2) & 3, c = p1 & 3, e = (p1 >> 2) & 3; \
                        p0 >>= 4; p1 >>= 4; \
                        if (smooth) { \
                            d0[0] = (T)lut[4 * a]; d0[1] = (T)lut[2 * (a + b)]; d0[2] = (T)lut[4 * b]; \
                            d1[0] = (T)lut[2 * (a + c)]; d1[1] = (T)lut[a + b + c + e]; d1[2] = (T)lut[2 * (b + e)]; \
                            d2[0] = (T)lut[4 * c]; d2[1] = (T)lut[2 * (c + e)]; d2[2] = (T)lut[4 * e]; \
                        } else { \
                            T ca = (T)lut[4 * a], cb = (T)lut[4 * b], cc = (T)lut[4 * c], ce = (T)lut[4 * e]; \
                            d0[0] = ca; d0[1] = ca; d0[2] = cb; \
                            d1[0] = ca; d1[1] = ca; d1[2] = cb; \
                            d2[0] = cc; d2[1] = cc; d2[2] = ce; \
                        } \
                        d0 += 3; d1 += 3; d2 += 3; \
                    } \
                } \
            } \
        } \
    } while (0)

static void sv_blit(sv_t *s, int all) {
    if (s->bpp == 16) {
        SV_BLIT(uint16_t);
    } else {
        SV_BLIT(uint8_t);
    }
}

static void sv_end_frame(sv_t *s) {
    int all = 0;
    if (s->dirty) {
        sv_render_lines(s);
        s->dirty = 0;
    }
    if (s->pal_dirty) {
        sv_build_lut(s);
        s->pal_dirty = 0;
        all = 1;
    }
    int any = all;
    for (uint32_t y = 0; y < SV_H && !any; y++) {
        any = s->changed[y];
    }
    if (any) {
        if (s->fb) {
            sv_blit(s, all);
        }
        if (s->crc_on) {
            s->last_crc = sv_crc32(s->line[0], sizeof(s->line));
        }
        for (uint32_t y = 0; y < SV_H; y++) {
            s->changed[y] = 0;
        }
    }
}

// Time-based events: NMI/frame, timer expiry, audio DMA end.
static void sv_events(sv_t *s) {
    uint32_t now = s->ticks;
    if (s->timer_on && (int32_t)(now - s->timer_end) >= 0) {
        s->timer_on = 0;
        s->timer_flag = 1;
    }
    if (s->adma_on && (int32_t)(now - s->adma_end) >= 0) {
        sv_audio_sync(s, now);  // plays out the sample and raises dma_flag
    }
    if ((int32_t)(now - s->next_frame) >= 0) {
        sv_audio_sync(s, s->next_frame);
        // Length counters run off the same 65536-cycle prescaler as the NMI.
        for (int k = 0; k < 2; k++) {
            if (s->sq[k].len) {
                s->sq[k].len--;
            }
        }
        if (s->noise_len) {
            s->noise_len--;
        }
        s->next_frame += SV_FRAME_TICKS;
        s->frames++;
        sv_end_frame(s);
        if (s->reg[0x26] & 1) {
            s->nmi_pending = 1;
        }
    }
    sv_update_irq(s);
    sv_set_next_event(s);
}

// ---- registers ---------------------------------------------------------------
static uint32_t sv_reg_index(uint16_t a) {
    uint32_t r = a & 0x3F;
    if (r >= 4 && r < 8) {
        r -= 4;
    } else if (r >= 0x2C && r < 0x2F) {
        r -= 4;
    }
    return r;
}

static uint8_t sv_io_read(sv_t *s, uint16_t a) {
    uint32_t r = sv_reg_index(a);
    switch (r) {
        case 0x20:
            return (uint8_t)~s->joy;
        case 0x22: {  // link port: DDR bit 1 = input, inputs float high
            uint8_t ddr = s->reg[0x21] & 15;
            return (uint8_t)(0xF0 | (s->reg[0x22] & ~ddr & 15) | ddr);
        }
        case 0x23:
            if (s->timer_on) {
                uint32_t left = s->timer_end - s->ticks;
                return (uint8_t)((left + s->timer_presc - 1) / s->timer_presc);
            }
            return 0;
        case 0x24:
            s->timer_flag = 0;
            sv_update_irq(s);
            break;
        case 0x25:
            s->dma_flag = 0;
            sv_update_irq(s);
            break;
        case 0x27:
            return (uint8_t)((s->reg[0x27] & ~3) | s->timer_flag | (s->dma_flag << 1));
        default:
            break;
    }
    return s->reg[r];
}

// VRAM DMA: (L ? L : 256) * 16 bytes, 5 bytes per 6 cycles with the CPU held.
// The address registers are left advanced and the length at 0, as measured.
static void sv_vdma(sv_t *s) {
    uint16_t src = (uint16_t)(s->reg[8] | (s->reg[9] << 8));
    uint16_t dst = (uint16_t)(s->reg[0x0A] | (s->reg[0x0B] << 8));
    uint32_t n = (s->reg[0x0C] ? s->reg[0x0C] : 256) * 16u;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t v = sv_peek(s, src++);
        if (dst >= 0x4000 && dst < 0x6000) {
            s->vram[dst - 0x4000] = v;
        } else if (dst < 0x2000) {
            s->wram[dst] = v;
        }
        dst++;
    }
    s->reg[8] = (uint8_t)src;
    s->reg[9] = (uint8_t)(src >> 8);
    s->reg[0x0A] = (uint8_t)dst;
    s->reg[0x0B] = (uint8_t)(dst >> 8);
    s->reg[0x0C] = 0;
    s->ticks += n * 6 / 5;
    s->dirty = 1;
}

static void sv_io_write(sv_t *s, uint16_t a, uint8_t v) {
    uint32_t r = sv_reg_index(a);
    if ((r >= 0x10 && r <= 0x1C) || (r >= 0x28 && r <= 0x2A)) {
        sv_audio_sync(s, s->ticks);
    }
    s->reg[r] = v;
    switch (r) {
        case 0x00: case 0x01: case 0x02: case 0x03:
            s->dirty = 1;
            break;
        case 0x0D:
            if (v & 0x80) {
                sv_vdma(s);
            }
            break;
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x14: case 0x15: case 0x16: case 0x17: {
            sv_square_t *q = &s->sq[(r - 0x10) >> 2];
            uint32_t o = r & 3;
            q->reg[o] = v;
            if (o < 2) {
                q->period = 32u * ((q->reg[0] | ((q->reg[1] & 7u) << 8)) + 1);
                q->phase = 0;
            } else if (o == 3) {
                q->len = (uint16_t)(v + 1);
            }
        } break;
        case 0x18: case 0x19:
            s->adma_addr = (uint16_t)(s->reg[0x18] | (s->reg[0x19] << 8));
            break;
        case 0x1B:
            if (s->adma_on) {
                sv_adma_schedule(s);
            }
            break;
        case 0x1C:
            if (v & 0x80) {
                uint32_t len = s->reg[0x1A] ? s->reg[0x1A] : 256;
                s->adma_nibbles = len * 32;
                s->adma_left = 256u << (s->reg[0x1B] & 3);
                s->adma_nib = 0;
                uint16_t ad = s->adma_addr;
                if (ad >= 0x8000 && ad < 0xC000) {
                    uint32_t b = ((s->reg[0x1B] >> 4) & 7) % s->banks;
                    s->adma_byte = s->rom[((2 * b + ((ad >> 13) & 1)) % s->rom_pages) * 0x2000 + (ad & 0x1FFF)];
                } else {
                    s->adma_byte = sv_peek(s, ad);
                }
                s->reg[0x1A] = 0;  // a retrigger plays the full 4096 bytes
                s->adma_on = 1;
                s->dma_flag = 0;
                sv_adma_schedule(s);
            } else {
                s->adma_on = 0;
            }
            break;
        case 0x23:
            s->timer_presc = (s->reg[0x26] & 0x10) ? 16384 : 256;
            if (v == 0) {  // instant IRQ, no wrap
                s->timer_on = 0;
                s->timer_flag = 1;
            } else {
                s->timer_on = 1;
                s->timer_end = s->ticks + v * s->timer_presc;
            }
            break;
        case 0x26:
            sv_map(s);
            s->dirty = 1;
            break;
        case 0x28:
            s->noise_div = sv_noise_div[v >> 4];
            break;
        case 0x29:
            s->noise_len = (uint16_t)(v + 1);
            break;
        case 0x2A:
            s->lfsr = 0x7FFF;
            s->noise_left = s->noise_div;
            break;
        default:
            break;
    }
    sv_update_irq(s);
    sv_set_next_event(s);
}

// ---- setup -------------------------------------------------------------------
static void sv_set_palette(sv_t *s, const uint32_t *rgb) {
    for (int i = 0; i < 4; i++) {
        s->pal[i] = rgb[i] & 0xFFFFFF;
    }
    s->pal_dirty = 1;
}

// 0 on success, 1 if the ROM is not a whole number of 8 KB pages (or empty).
static int sv_init(sv_t *s, const uint8_t *rom, uint32_t len) {
    uint8_t *p = (uint8_t *)s;
    for (uint32_t i = 0; i < sizeof(sv_t); i++) {
        p[i] = 0;
    }
    if (len < 0x2000 || (len & 0x1FFF)) {
        return 1;
    }
    s->rom = rom;
    s->rom_size = len;
    s->rom_pages = len / 0x2000;
    s->banks = s->rom_pages >= 2 ? s->rom_pages / 2 : 1;
    s->page[0] = s->wram;
    s->page[2] = s->vram;
    sv_map(s);
    static const uint32_t green[4] = {0xC4CFA1, 0x8B956D, 0x4D533C, 0x1F1F1F};
    sv_set_palette(s, green);
    s->reg[0] = s->reg[1] = 0xA0;
    for (int k = 0; k < 2; k++) {
        s->sq[k].period = 32;
    }
    s->noise_div = 8;
    s->noise_left = 8;
    s->lfsr = 0x7FFF;
    s->timer_presc = 256;
    s->next_frame = SV_FRAME_TICKS;
    s->next_sample = SV_SAMPLE_TICKS;
    s->S = 0xFD;
    s->I_ = 1;
    s->Z_ = 1;
    s->PC = (uint16_t)(s->page[7][0x1FFC] | (s->page[7][0x1FFD] << 8));
    s->dirty = 1;
    sv_set_next_event(s);
    return 0;
}

// ---- CPU loop ----------------------------------------------------------------
// Everything but WRAM writes and mapped reads, kept out of line so the
// opcode switch stays small (on the board the code runs through a 16 KB cache).
static __attribute__((noinline)) uint8_t sv_read_slow(sv_t *s, uint16_t a) {
    if (a >= 0x2000 && a < 0x4000) {
        return sv_io_read(s, a);
    }
    return 0xFF;  // $6000-$7FFF: nothing there
}

static __attribute__((noinline)) void sv_write_slow(sv_t *s, uint16_t a, uint8_t v) {
    if (a < 0x4000) {
        sv_io_write(s, a, v);
    } else if (a < 0x6000) {
        s->vram[a - 0x4000] = v;
        s->dirty = 1;
    }
}

// Run until `frames` more NMI periods have passed (or the CPU executes STP).
static void sv_run(sv_t *s, uint32_t frames) {
    uint8_t A = s->A, X = s->X, Y = s->Y, S = s->S;
    uint16_t PC = s->PC;
    unsigned C_ = s->C_, V_ = s->V_, D_ = s->D_, I_ = s->I_, N_ = s->N_, Z_ = s->Z_;
    uint32_t target = s->frames + frames;
    uint8_t *const wram = s->wram;
    uint32_t ticks = s->ticks, next_event = s->next_event;
    int halted = s->wai;
    s->samples = 0;
    // Hot state in locals: WRAM stores through a uint8_t pointer may alias
    // anything in *s, and *s may sit in slow PSRAM on the board.
    const uint8_t *pg[8];
    for (int i = 0; i < 8; i++) {
        pg[i] = s->page[i];
    }

#define STATE_IN() do { ticks = s->ticks; next_event = s->next_event; \
        pg[4] = s->page[4]; pg[5] = s->page[5]; } while (0)
#define PUSH_IRQ(vec, brk) do { \
        uint8_t p_ = (uint8_t)((N_ & 0x80) | (V_ << 6) | 0x20 | (brk) | (D_ << 3) | (I_ << 2) | (Z_ ? 0 : 2) | C_); \
        wram[0x100 | S] = (uint8_t)(PC >> 8); S--; \
        wram[0x100 | S] = (uint8_t)PC; S--; \
        wram[0x100 | S] = p_; S--; \
        I_ = 1; D_ = 0; \
        PC = (uint16_t)(pg[7][(vec) & 0x1FFF] | (pg[7][((vec) + 1) & 0x1FFF] << 8)); \
        ticks += 7; } while (0)

    for (;;) {
        if (halted && (int32_t)(ticks - next_event) < 0) {
            ticks = next_event;
        }
        if ((int32_t)(ticks - next_event) >= 0) {
            s->ticks = ticks;
            sv_events(s);
            STATE_IN();
            if (s->frames == target || s->stopped) {
                break;
            }
        }
        if (s->nmi_pending) {
            s->nmi_pending = 0;
            halted = s->wai = 0;
            PUSH_IRQ(0xFFFA, 0);
            continue;
        }
        if (s->irq) {
            if (halted) {
                halted = s->wai = 0;
            }
            if (!I_) {
                PUSH_IRQ(0xFFFE, 0);
                continue;
            }
        }
        if (halted) {
            continue;
        }
#ifdef SV_HOOK
        SV_HOOK(s, PC);
#endif
        unsigned cyc = 0;
#define RD(a) __extension__({ uint16_t a_ = (uint16_t)(a); uint8_t v_; cyc++; \
        const uint8_t *p_ = pg[a_ >> 13]; \
        if (p_) { v_ = p_[a_ & 0x1FFF]; } \
        else { s->ticks = ticks + cyc; cyc = 0; v_ = sv_read_slow(s, a_); STATE_IN(); } v_; })
#define WR(a, v) do { uint16_t a_ = (uint16_t)(a); uint8_t v_ = (uint8_t)(v); cyc++; \
        if (a_ < 0x2000) { wram[a_] = v_; } \
        else { s->ticks = ticks + cyc; cyc = 0; sv_write_slow(s, a_, v_); STATE_IN(); } } while (0)
#define TICK() (cyc++)
#define WAIT() (s->wai = 1, halted = 1)
#define STOP() (s->stopped = 1, s->err = SV_ERR_STP, next_event = ticks)
#define CPU_ROCKWELL 1
#include "cpu65c02.h"
#undef CPU_ROCKWELL
#undef RD
#undef WR
#undef TICK
#undef WAIT
#undef STOP
        ticks += cyc;
    }
    s->ticks = ticks;
    s->A = A; s->X = X; s->Y = Y; s->S = S; s->PC = PC;
    s->C_ = C_; s->V_ = V_; s->D_ = D_; s->I_ = I_; s->N_ = N_; s->Z_ = Z_;
#undef STATE_IN
#undef PUSH_IRQ
}
