// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-FileCopyrightText: 1996-2004 K. Wilkins (Handy)
// SPDX-License-Identifier: Zlib
//
// Atari Lynx core in plain C for the `lynx` native module.
//
// This is an altered source version of parts of Handy by K. Wilkins
// (zlib license, see LICENSE.handy): the Suzy sprite engine and math unit,
// the Mikey timer, display DMA and register behavior, and the cartridge
// shifter/counter are ports of Handy's susie.cpp, mikie.cpp and cart.cpp,
// rewritten from C++ classes into one C struct with no globals, statics or
// allocation. It is NOT the original software. What is new here: the event
// loop, audio (channels advanced lazily and box-filtered to 22050 Hz rather
// than every timer event going through blip_buf), the 65C02 (cpu65c02.h, MIT,
// ours), the HLE boot (decryption written from the published algorithm, not
// copied), and the framebuffer output.
//
// All state is in lynx_t, which the caller allocates (a Python bytearray).
// Time is counted in 16 MHz system ticks like Handy: a CPU cycle costs 5
// ticks, minus 4 per instruction; display DMA steals 720 ticks per line;
// sprite painting sleeps the CPU 3 ticks per Suzy bus access.

#define LYNX_W 160
#define LYNX_H 102
#define LYNX_AUDIO_RATE 22050
#define LYNX_TICKS_PER_SEC 16000000u
#define LYNX_SAMPLE_TICKS (LYNX_TICKS_PER_SEC / LYNX_AUDIO_RATE)  // 725
#define LYNX_SAMPLE_FRAC (LYNX_TICKS_PER_SEC % LYNX_AUDIO_RATE)   // 13750

// Joypad bits as the JOYSTICK register reads them in normal (right handed)
// orientation, plus PAUSE (SWITCHES bit 0) at bit 8.
#define LYNX_A 0x01      // outer button
#define LYNX_B 0x02      // inner button
#define LYNX_OPT2 0x04
#define LYNX_OPT1 0x08
#define LYNX_RIGHT 0x10
#define LYNX_LEFT 0x20
#define LYNX_DOWN 0x40
#define LYNX_UP 0x80
#define LYNX_PAUSE 0x100

enum { BOOT_NONE, BOOT_HLE, BOOT_BS93, BOOT_ROM };

typedef struct {
    int32_t cur;     // counts down; negative means it just underflowed
    uint32_t last;   // tick of the last decrement (own clocked mode)
    uint8_t bkup;
    uint8_t reload, count, linking, done, borrow_in, borrow_out, last_clock;
} ltimer_t;

typedef struct {
    ltimer_t t;
    uint32_t ws;       // bits 0-11 LFSR, 12-20 feedback taps 7,0,1,2,3,4,5,10,11
    int8_t vol;
    int8_t out;
    uint8_t integrate;
    uint8_t pad;
    int32_t acc;       // sum of out * ticks since acc_t0
    uint32_t acc_t;    // tick acc is integrated up to
} achan_t;

typedef struct lynx_s {
    uint8_t ram[65536];
    uint8_t rom[512];          // HLE stub ROM, or a copy of lynxboot.img
    const uint8_t *cart;       // bank 0 data (after the .lnx header)
    uint32_t cart_len;
    uint32_t bank0_mask, bank0_shift, bank0_count_mask;
    uint32_t cart_counter, cart_shifter;
    uint8_t cart_strobe, cart_last_strobe, cart_addr_data;

    // CPU
    uint8_t A, X, Y, S;
    uint16_t PC;
    uint8_t C_, V_, D_, I_, N_, Z_;
    uint8_t sleeping, wai, stopped, hle;
    uint8_t mapctl;
    uint8_t boot, rotation, err;
    uint32_t ticks;
    uint32_t next_event;       // min of timer_event, wake_time, next_sample
    uint32_t timer_event;
    uint32_t wake_time;        // 0 = none
    uint32_t boot_blocks;
    uint16_t boot_addr;

    // Mikey
    ltimer_t tim[8];
    achan_t aud[4];
    uint8_t irq_flags, irq_mask;
    uint8_t stereo, pan, atten[4];
    uint16_t palette[16];      // 12-bit: green bits 0-3, red 4-7, blue 8-11
    uint8_t pal_dirty;
    uint8_t iodir, iodat, rest;
    uint8_t dma_enable, flip;
    uint16_t disp_addr;
    uint32_t lynx_line, dma_lines;
    uint32_t lynx_addr;
    uint32_t frames;           // end of frame count
    // ComLynx UART, no cable: what is sent loops back, as on hardware.
    uint32_t uart_tx_end;      // tick the byte in flight is done, 0 = idle
    uint8_t uart_txien, uart_rxien, uart_rx_ready, uart_rx_data, uart_tx_data;

    // Suzy
    uint16_t tmpadr, tiltacum, hoff, voff, vidbas, collbas, vidadr, colladr;
    uint16_t scbnext, sprdline, hposstrt, vposstrt, sprhsiz, sprvsiz;
    uint16_t stretch, tilt, sprdoff, sprvpos, colloff, vsizacum, hsizacum;
    uint16_t hsizoff, vsizoff, scbadr, procadr;
    uint32_t math_abcd, math_efgh, math_jklm, math_np;
    int8_t math_ab_sign, math_cd_sign, math_efgh_sign;
    uint8_t spr_type, spr_vflip, spr_hflip, spr_bits;
    uint8_t spr_left, spr_up, spr_skip, spr_reload_pal, spr_reload_depth, spr_sizing, spr_literal;
    uint8_t coll_num, coll_off;
    uint8_t sys_stop, sys_lefthand, sys_vstretch, sys_nocollide, sys_accumulate, sys_signed;
    uint8_t sys_unsafe, sys_lastcarry, sys_mathbit;
    uint8_t busen, sprgo, everon, sprinit;
    uint8_t pen[16];
    uint32_t line_type, line_sr_count, line_sr, line_repeat, line_pixel, line_bits_left;
    uint32_t collision;
    uint32_t line_base, line_coll;
    uint32_t spr_cycles;
    uint32_t spr_painted;      // sprites painted, for debugging
    uint16_t joy;              // LYNX_* bits

    // Audio output
    uint32_t next_sample;      // tick of the next sample boundary
    uint32_t sample_frac;
    int32_t dc_l, dc_r;        // DC trackers, << 8
    int16_t *ring;             // stereo int16 ring, or NULL
    uint32_t ring_frames, ring_pos;
    uint32_t samples;          // samples made since the last run call

    // Video output
    uint8_t *fb;
    uint32_t fb_width, bpp, x0, y0;
    uint32_t lut32[16];        // RGB565 pixel doubled: c | c << 16
    uint16_t lut8x2[16];       // RGB332 pixel doubled
    uint32_t crc_on, crc, last_crc;
} lynx_t;

static const uint32_t lynx_crc_nibble[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

static uint32_t lynx_crc32(uint32_t c, const uint8_t *p, uint32_t len) {
    while (len--) {
        c ^= *p++;
        c = (c >> 4) ^ lynx_crc_nibble[c & 15];
        c = (c >> 4) ^ lynx_crc_nibble[c & 15];
    }
    return c;
}

// ---------------------------------------------------------------------------
// Cartridge: bank 0 only (no AUDIN banking, no cart RAM/EEPROM).

static uint8_t cart_read(lynx_t *l) {
    uint32_t a = ((l->cart_shifter << l->bank0_shift) + (l->cart_counter & l->bank0_count_mask)) & l->bank0_mask;
    uint8_t v = a < l->cart_len ? l->cart[a] : 0xFF;
    if (!l->cart_strobe) {
        l->cart_counter = (l->cart_counter + 1) & 0x7FF;
    }
    return v;
}

static void cart_strobe(lynx_t *l, int strobe) {
    l->cart_strobe = (uint8_t)strobe;
    if (strobe) {
        l->cart_counter = 0;
    }
    if (strobe && !l->cart_last_strobe) {
        l->cart_shifter = ((l->cart_shifter << 1) | (l->cart_addr_data ? 1 : 0)) & 0xFF;
    }
    l->cart_last_strobe = (uint8_t)strobe;
}

// ---------------------------------------------------------------------------
// HLE boot: the boot ROM decrypts the cart's first "frame" (a count byte then
// blocks of 51 bytes, each c^3 mod N with the published public modulus) into
// RAM at $0200 and jumps there.

#define LYNX_KEY_LEN 51
static const uint8_t lynx_modulus[LYNX_KEY_LEN] = {  // big endian
    0x35, 0xB5, 0xA3, 0x94, 0x28, 0x06, 0xD8, 0xA2, 0x26, 0x95, 0xD7, 0x71, 0xB2, 0x3C, 0xFD, 0x56,
    0x1C, 0x4A, 0x19, 0xB6, 0xA3, 0xB0, 0x26, 0x00, 0x36, 0x5A, 0x30, 0x6E, 0x3C, 0x4D, 0x63, 0x38,
    0x1B, 0xD4, 0x1C, 0x13, 0x64, 0x89, 0x36, 0x4C, 0xF2, 0xBA, 0x2A, 0x58, 0xF4, 0xFE, 0xE1, 0xFD,
    0xAC, 0x7E, 0x79,
};

// r -= m if r >= m (big endian bytes)
static void bn_reduce(uint8_t *r) {
    uint8_t t[LYNX_KEY_LEN];
    int borrow = 0;
    for (int i = LYNX_KEY_LEN - 1; i >= 0; i--) {
        int d = r[i] - lynx_modulus[i] - borrow;
        borrow = d < 0;
        t[i] = (uint8_t)d;
    }
    if (!borrow) {
        memcpy(r, t, LYNX_KEY_LEN);
    }
}

// r = a * b mod m, a and b < m, by double and add.
static void bn_mulmod(uint8_t *r, const uint8_t *a, const uint8_t *b) {
    memset(r, 0, LYNX_KEY_LEN);
    for (int i = 0; i < LYNX_KEY_LEN; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            int c = 0;
            for (int k = LYNX_KEY_LEN - 1; k >= 0; k--) {
                int v = (r[k] << 1) | c;
                r[k] = (uint8_t)v;
                c = v >> 8;
            }
            bn_reduce(r);
            if ((b[i] >> bit) & 1) {
                c = 0;
                for (int k = LYNX_KEY_LEN - 1; k >= 0; k--) {
                    int v = r[k] + a[k] + c;
                    r[k] = (uint8_t)v;
                    c = v >> 8;
                }
                bn_reduce(r);
            }
        }
    }
}

// Decrypt one frame read from the cart, writing 50 bytes per block to RAM at
// addr. Returns the number of blocks.
static uint32_t hle_load_frame(lynx_t *l, uint16_t addr) {
    uint32_t blocks = 256 - cart_read(l);
    uint8_t acc = 0;
    for (uint32_t b = 0; b < blocks; b++) {
        uint8_t c[LYNX_KEY_LEN], sq[LYNX_KEY_LEN], cube[LYNX_KEY_LEN];
        for (int i = LYNX_KEY_LEN - 1; i >= 0; i--) {
            c[i] = cart_read(l);  // stored little endian
        }
        bn_reduce(c);
        bn_mulmod(sq, c, c);
        bn_mulmod(cube, sq, c);
        for (int i = LYNX_KEY_LEN - 1; i > 0; i--) {
            acc = (uint8_t)(acc + cube[i]);
            l->ram[addr++] = acc;
        }
    }
    return blocks;
}

// ---------------------------------------------------------------------------
// Mikey

static void mikey_update(lynx_t *l);
static uint32_t lynx_paint_sprites(lynx_t *l);
static void audio_sync(lynx_t *l, uint32_t t);

static void pal_to_lut(lynx_t *l) {
    for (int i = 0; i < 16; i++) {
        uint32_t c = l->palette[i];
        uint32_t g = c & 15, r = (c >> 4) & 15, b = (c >> 8) & 15;
        uint32_t c16 = ((r << 1 | r >> 3) << 11) | ((g << 2 | g >> 2) << 5) | (b << 1 | b >> 3);
        uint32_t c8 = ((r >> 1) << 5) | ((g >> 1) << 2) | (b >> 2);
        l->lut32[i] = c16 | (c16 << 16);
        l->lut8x2[i] = (uint16_t)(c8 | (c8 << 8));
    }
    l->pal_dirty = 0;
}

// One display line of DMA at the line timer (timer 0) underflow. Returns
// ticks stolen from the CPU.
static uint32_t display_line(lynx_t *l) {
    uint32_t bk = l->tim[2].bkup;
    l->rest = (l->lynx_line == bk - 2 || l->lynx_line == bk - 3 || l->lynx_line == bk - 4);
    if (l->lynx_line == bk - 3) {
        l->lynx_addr = (l->disp_addr & 0xFFFC) + (l->flip ? 3 : 0);
        l->dma_lines = LYNX_H;
    }
    if (l->lynx_line) {
        l->lynx_line--;
    }
    if (!l->dma_lines || !l->dma_enable) {
        return 0;
    }
    uint32_t y = LYNX_H - l->dma_lines;
    l->dma_lines--;
    uint32_t a = l->lynx_addr;
    const uint8_t *ram = l->ram;
    // Flip reads the line backwards with the low nibble first.
    int step = l->flip ? -1 : 1;
    int sh0 = l->flip ? 0 : 4, sh1 = 4 - sh0;
    if (l->crc_on) {
        uint8_t px[LYNX_W * 2];
        uint32_t b = a;
        for (int x = 0; x < LYNX_W; x += 2) {
            uint8_t v = ram[b & 0xFFFF];
            b += step;
            uint16_t c0 = l->palette[(v >> sh0) & 15], c1 = l->palette[(v >> sh1) & 15];
            px[2 * x] = (uint8_t)c0;
            px[2 * x + 1] = (uint8_t)(c0 >> 8);
            px[2 * x + 2] = (uint8_t)c1;
            px[2 * x + 3] = (uint8_t)(c1 >> 8);
        }
        if (y == 0) {
            l->crc = 0xFFFFFFFF;
        }
        l->crc = lynx_crc32(l->crc, px, sizeof(px));
        if (y == LYNX_H - 1) {
            l->last_crc = ~l->crc;
        }
    }
    if (l->fb) {
        if (l->pal_dirty) {
            pal_to_lut(l);
        }
        // 2x: each pixel twice across, each line twice down, both rows at once.
        if (l->bpp == 16) {
            uint32_t *d0 = (uint32_t *)((uint16_t *)l->fb + (l->y0 + 2 * y) * l->fb_width + l->x0);
            uint32_t *d1 = d0 + l->fb_width / 2;
            const uint32_t *lut = l->lut32;
            for (int x = 0; x < LYNX_W; x += 2) {
                uint8_t v = ram[a & 0xFFFF];
                a += step;
                uint32_t p0 = lut[(v >> sh0) & 15], p1 = lut[(v >> sh1) & 15];
                d0[x] = p0;
                d0[x + 1] = p1;
                d1[x] = p0;
                d1[x + 1] = p1;
            }
        } else {
            uint16_t *d0 = (uint16_t *)(l->fb + (l->y0 + 2 * y) * l->fb_width + l->x0);
            uint16_t *d1 = d0 + l->fb_width / 2;
            const uint16_t *lut = l->lut8x2;
            for (int x = 0; x < LYNX_W; x += 2) {
                uint8_t v = ram[a & 0xFFFF];
                a += step;
                uint16_t p0 = lut[(v >> sh0) & 15], p1 = lut[(v >> sh1) & 15];
                d0[x] = p0;
                d0[x + 1] = p1;
                d1[x] = p0;
                d1[x + 1] = p1;
            }
        }
    }
    l->lynx_addr = l->lynx_addr + step * (LYNX_W / 2);
    return (80 + 100) * 4;
}

static void end_of_frame(lynx_t *l) {
    l->dma_lines = 0;
    l->lynx_line = l->tim[2].bkup;
    if (l->wake_time) {
        l->wake_time = 0;
        l->sleeping = 0;
    }
    l->frames++;
}

// Taps (switch bit -> LFSR bit): 0->7, 1..6 -> 0..5, 7->10, 8->11.
static inline uint32_t lfsr_next(uint32_t ws) {
    uint32_t sw = ws >> 12, lfsr = ws & 0xFFF;
    uint32_t mask = ((sw & 1) << 7) | ((sw >> 1) & 0x3F) | (((sw >> 7) & 3) << 10);
    uint32_t x = lfsr & mask;
    x ^= x >> 8;
    x ^= x >> 4;
    x ^= x >> 2;
    x ^= x >> 1;
    return (sw << 12) | ((lfsr << 1) & 0xFFE) | ((x & 1) ^ 1);
}

static void achan_clock(lynx_t *l, int k, uint32_t t);

// Channel k underflowed at tick t.
static void achan_underflow(lynx_t *l, int k, uint32_t t) {
    achan_t *a = &l->aud[k];
    a->acc += a->out * (int32_t)(t - a->acc_t);
    a->acc_t = t;
    a->t.borrow_out = 1;
    if (a->t.reload) {
        a->t.cur += a->t.bkup + 1;
        if (a->t.cur < 0) {
            a->t.cur = 0;
        }
    } else {
        a->t.done = 1;
        a->t.cur = 0;
    }
    if (a->t.bkup || a->t.linking) {
        a->ws = lfsr_next(a->ws);
    }
    if (a->integrate) {
        int32_t v = a->out + ((a->ws & 1) ? a->vol : -a->vol);
        a->out = (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v);
    } else {
        a->out = (a->ws & 1) ? a->vol : (int8_t)-a->vol;
    }
    if (k < 3 && l->aud[k + 1].t.linking == 7) {
        achan_clock(l, k + 1, t);
    }
}

// One borrow from the previous timer in the chain, for a linked channel.
static void achan_clock(lynx_t *l, int k, uint32_t t) {
    achan_t *a = &l->aud[k];
    if (!a->t.count || (!a->t.reload && a->t.done)) {
        return;
    }
    if (--a->t.cur < 0) {
        achan_underflow(l, k, t);
    }
}

// Run own-clocked channel k up to tick t.
static void achan_advance(lynx_t *l, int k, uint32_t t) {
    achan_t *a = &l->aud[k];
    if (!a->t.count || a->t.linking == 7 || (!a->t.reload && a->t.done)) {
        return;
    }
    if ((int32_t)(t - a->t.last) <= 0) {
        return;
    }
    uint32_t div = 4 + a->t.linking;
    uint32_t n = (t - a->t.last) >> div;
    if (!n) {
        return;
    }
    int chained = k < 3 && l->aud[k + 1].t.linking == 7 && l->aud[k + 1].t.count;
    if (!a->t.bkup && !a->t.linking && !chained) {
        // 1 MHz with backup 0: an underflow every clock but the LFSR does not
        // step, so the level settles at once.
        a->acc += a->out * (int32_t)(a->t.last - a->acc_t);
        a->acc_t = a->t.last;
        int32_t v = (a->ws & 1) ? a->vol : -a->vol;
        if (a->integrate) {
            v = a->out + v * (int32_t)(n > 256 ? 256 : n);
            v = v > 127 ? 127 : v < -128 ? -128 : v;
        }
        a->out = (int8_t)v;
        a->t.cur = 0;
        a->t.last += n << div;
        return;
    }
    if (a->t.reload && !a->integrate && !chained) {
        // Fast path for the usual case: step the LFSR once per underflow and
        // integrate the level, without the general bookkeeping.
        if (a->t.cur < 0) {
            a->t.cur = 0;
        }
        uint32_t need = (uint32_t)a->t.cur + 1;
        if (n < need) {
            a->t.cur -= (int32_t)n;
            a->t.last += n << div;
            return;
        }
        uint32_t period = (uint32_t)a->t.bkup + 1;
        uint32_t steps = 1 + (n - need) / period;
        uint32_t t1 = a->t.last + (need << div);
        a->acc += a->out * (int32_t)(t1 - a->acc_t);
        uint32_t ws = a->ws;
        int32_t vol = a->vol, sum = 0, lvl = 0;
        for (uint32_t i = 0; i < steps; i++) {
            ws = lfsr_next(ws);
            lvl = (ws & 1) ? vol : -vol;
            sum += lvl;
        }
        a->ws = ws;
        a->acc += (sum - lvl) * (int32_t)(period << div);
        a->out = (int8_t)lvl;
        a->acc_t = t1 + ((steps - 1) * period << div);
        a->t.cur = (int32_t)(a->t.bkup - (n - need) % period);
        a->t.last += n << div;
        a->t.borrow_out = 1;
        return;
    }
    while (n) {
        if (a->t.cur < 0) {
            a->t.cur = 0;
        }
        uint32_t need = (uint32_t)a->t.cur + 1;
        if (n < need) {
            a->t.cur -= (int32_t)n;
            a->t.last += n << div;
            break;
        }
        a->t.last += need << div;
        n -= need;
        a->t.cur = -1;
        achan_underflow(l, k, a->t.last);
        if (!a->t.reload) {
            break;
        }
    }
}

// Bring all channels to tick t.
static void audio_sync(lynx_t *l, uint32_t t) {
    for (int k = 0; k < 4; k++) {
        achan_advance(l, k, t);
    }
}

// Emit one output sample at the boundary l->next_sample.
static void audio_sample(lynx_t *l) {
    uint32_t t = l->next_sample;
    audio_sync(l, t);
    int32_t left = 0, right = 0;
    for (int k = 0; k < 4; k++) {
        achan_t *a = &l->aud[k];
        a->acc += a->out * (int32_t)(t - a->acc_t);
        a->acc_t = t;
        int32_t v = (a->acc * 5786) >> 22;  // mean level over the ~725 tick sample
        a->acc = 0;
        if (!(l->stereo & (0x10 << k))) {
            left += (l->pan & (0x10 << k)) ? v * (l->atten[k] & 0xF0) / 256 : v;
        }
        if (!(l->stereo & (0x01 << k))) {
            right += (l->pan & (0x01 << k)) ? v * (l->atten[k] & 0x0F) / 16 : v;
        }
    }
    // Remove DC (channels often idle at +-volume), scale 4 x 8 bit to 16 bit.
    left <<= 8;
    right <<= 8;
    l->dc_l += (left - l->dc_l) >> 10;
    l->dc_r += (right - l->dc_r) >> 10;
    int32_t sl = ((left - l->dc_l) >> 8) * 64;
    int32_t sr = ((right - l->dc_r) >> 8) * 64;
    sl = sl > 32767 ? 32767 : sl < -32768 ? -32768 : sl;
    sr = sr > 32767 ? 32767 : sr < -32768 ? -32768 : sr;
    if (l->ring) {
        l->ring[l->ring_pos * 2] = (int16_t)sl;
        l->ring[l->ring_pos * 2 + 1] = (int16_t)sr;
        if (++l->ring_pos == l->ring_frames) {
            l->ring_pos = 0;
        }
    }
    l->samples++;
    l->next_sample += LYNX_SAMPLE_TICKS;
    l->sample_frac += LYNX_SAMPLE_FRAC;
    if (l->sample_frac >= LYNX_AUDIO_RATE) {
        l->sample_frac -= LYNX_AUDIO_RATE;
        l->next_sample++;
    }
}

static void set_next_event(lynx_t *l) {
    uint32_t e = l->timer_event;
    if ((int32_t)(l->next_sample - e) < 0) {
        e = l->next_sample;
    }
    if (l->wake_time && (int32_t)(l->wake_time - e) < 0) {
        e = l->wake_time;
    }
    l->next_event = e;
}

// Generic timer step, following Handy's Update(). prev_borrow is the borrow
// out of the previous timer in the chain (for linking == 7). Returns 1 on an
// underflow.
static int timer_step(lynx_t *l, ltimer_t *t, int prev_borrow, int irq_bit, int always_reload, int can_link) {
    if (!t->count || (!always_reload && !t->reload && t->done)) {
        return 0;
    }
    uint32_t div = 4 + t->linking;
    int linked = can_link && t->linking == 7;
    int32_t dec;
    if (linked) {
        dec = prev_borrow;
    } else {
        dec = (int32_t)((l->ticks - t->last) >> div);
    }
    int under = 0;
    if (dec) {
        if (!linked) {
            t->last += (uint32_t)dec << div;
        }
        t->cur -= dec;
        if (t->cur < 0) {
            under = 1;
            t->borrow_out = 1;
            if (irq_bit & l->irq_mask) {
                l->irq_flags |= (uint8_t)irq_bit;
            }
            if (always_reload || t->reload) {
                t->cur += t->bkup + 1;
            } else {
                t->cur = 0;
            }
            t->done = 1;
        } else {
            t->borrow_out = 0;
        }
        t->borrow_in = 1;
    } else {
        t->borrow_in = 0;
        t->borrow_out = 0;
    }
    if (!linked) {
        uint32_t next = l->ticks + (t->cur < 0 ? 1 : ((uint32_t)(t->cur + 1) << div));
        if ((int32_t)(next - l->timer_event) < 0) {
            l->timer_event = next;
        }
    }
    return under;
}

// UART interrupts are level triggered: TX idle with TXINTEN, or a byte
// waiting with RXINTEN, keeps the timer 4 (serial) flag set.
static void uart_update(lynx_t *l) {
    if (l->uart_tx_end && (int32_t)(l->ticks - l->uart_tx_end) >= 0) {
        l->uart_tx_end = 0;
        l->uart_rx_data = l->uart_tx_data;  // loopback
        l->uart_rx_ready = 1;
    }
    if ((!l->uart_tx_end && l->uart_txien) || (l->uart_rx_ready && l->uart_rxien)) {
        l->irq_flags |= 0x10;
    }
    if (l->uart_tx_end && (int32_t)(l->uart_tx_end - l->timer_event) < 0) {
        l->timer_event = l->uart_tx_end;
    }
}

static void mikey_update(lynx_t *l) {
    uint32_t work = 0;
    l->timer_event = l->ticks + 0x40000000;
    if (l->wake_time && (int32_t)(l->ticks - l->wake_time) >= 0) {
        l->sleeping = 0;
        l->wake_time = 0;
    }
    // Group A: timer 0 (line) -> timer 2 (frame) -> timer 4 (UART, ignored).
    if (timer_step(l, &l->tim[0], 0, 0x01, 1, 0)) {
        work += display_line(l);
    }
    if (l->tim[2].count) {
        // Frame timer: always linked to the line timer, as in Handy.
        ltimer_t *t = &l->tim[2];
        if (l->tim[0].borrow_out) {
            if (--t->cur < 0) {
                t->borrow_out = 1;
                t->cur += t->bkup + 1;
                t->done = 1;
                if (l->irq_mask & 0x04) {
                    l->irq_flags |= 0x04;
                }
                end_of_frame(l);
            } else {
                t->borrow_out = 0;
            }
            t->borrow_in = 1;
        } else {
            t->borrow_in = t->borrow_out = 0;
        }
    }
    // Group B: 1 -> 3 -> 5 -> 7 -> audio 0 -> 1 -> 2 -> 3.
    int b = timer_step(l, &l->tim[1], 0, 0x02, 0, 1);
    b = timer_step(l, &l->tim[3], b, 0x08, 0, 1);
    b = timer_step(l, &l->tim[5], b, 0x20, 0, 1);
    b = timer_step(l, &l->tim[7], b, 0x80, 0, 1);
    // Timer 6 is in no chain; linking 7 is just its slowest clock.
    timer_step(l, &l->tim[6], 0, 0x40, 0, 0);
    if (b && l->aud[0].t.linking == 7) {
        audio_sync(l, l->ticks);
        achan_clock(l, 0, l->ticks);
    }
    uart_update(l);
    if (l->irq_flags && l->sleeping) {
        l->sleeping = 0;
    }
    l->ticks += work;
    set_next_event(l);
}

static void timer_ctla(lynx_t *l, ltimer_t *t, int irq_bit, uint8_t v) {
    if (irq_bit) {
        l->irq_mask = (uint8_t)((l->irq_mask & ~irq_bit) | ((v & 0x80) ? irq_bit : 0));
    }
    t->reload = (v >> 4) & 1;
    t->count = (v >> 3) & 1;
    t->linking = v & 7;
    if (v & 0x40) {
        t->done = 0;
    }
    if (v & 0x48) {
        t->last = l->ticks;
    }
}

static uint8_t timer_ctlb(const ltimer_t *t) {
    return (uint8_t)((t->done ? 8 : 0) | (t->last_clock ? 4 : 0) | (t->borrow_in ? 2 : 0) | (t->borrow_out ? 1 : 0));
}

static void mikey_write(lynx_t *l, uint16_t addr, uint8_t v) {
    uint32_t r = addr & 0xFF;
    if (r < 0x20) {
        ltimer_t *t = &l->tim[r >> 2];
        switch (r & 3) {
            case 0: t->bkup = v; break;
            case 1: timer_ctla(l, t, (r >> 2) == 4 ? 0 : 1 << (r >> 2), v); break;
            case 2: t->cur = v; break;
            case 3:
                t->done = (v >> 3) & 1;
                t->last_clock = (v >> 2) & 1;
                t->borrow_in = (v >> 1) & 1;
                t->borrow_out = v & 1;
                break;
        }
        l->timer_event = l->ticks;
        l->next_event = l->ticks;
        return;
    }
    if (r < 0x40) {
        audio_sync(l, l->ticks);
        achan_t *a = &l->aud[(r - 0x20) >> 3];
        switch (r & 7) {
            case 0: a->vol = (int8_t)v; break;
            case 1: a->ws = (a->ws & 0x001FFF) | ((uint32_t)v << 13); break;
            case 2: a->acc += a->out * (int32_t)(l->ticks - a->acc_t); a->acc_t = l->ticks; a->out = (int8_t)v; break;
            case 3: a->ws = (a->ws & 0x1FFF00) | v; break;
            case 4:
                if (!a->t.bkup && v) {
                    a->t.last = l->ticks;
                }
                a->t.bkup = v;
                break;
            case 5:
                a->t.reload = (v >> 4) & 1;
                a->t.count = (v >> 3) & 1;
                a->t.linking = v & 7;
                a->integrate = (v >> 5) & 1;
                if (v & 0x40) {
                    a->t.done = 0;
                }
                a->ws = (a->ws & 0x1FEFFF) | ((v & 0x80) ? 0x1000 : 0);
                if (v & 0x48) {
                    a->t.last = l->ticks;
                }
                break;
            case 6: a->t.cur = v; break;
            case 7:
                a->ws = (a->ws & 0x1FF0FF) | ((uint32_t)(v & 0xF0) << 4);
                a->t.borrow_in = (v >> 1) & 1;
                a->t.borrow_out = v & 1;
                a->t.last_clock = (v >> 2) & 1;
                break;
        }
        return;
    }
    switch (r) {
        case 0x40: case 0x41: case 0x42: case 0x43: audio_sync(l, l->ticks); l->atten[r - 0x40] = v; break;
        case 0x44: audio_sync(l, l->ticks); l->pan = v; break;
        case 0x50: audio_sync(l, l->ticks); l->stereo = v; break;
        case 0x80:  // INTRST
            l->irq_flags &= (uint8_t)~v;
            l->timer_event = l->next_event = l->ticks;
            break;
        case 0x81:  // INTSET
            l->irq_flags |= v;
            l->timer_event = l->next_event = l->ticks;
            break;
        case 0x87:  // SYSCTL1: bit 0 cart address strobe
            cart_strobe(l, v & 1);
            break;
        case 0x8A: l->iodir = v; break;
        case 0x8C:  // SERCTL
            l->uart_txien = (v >> 7) & 1;
            l->uart_rxien = (v >> 6) & 1;
            uart_update(l);
            set_next_event(l);
            break;
        case 0x8D: {  // SERDAT: 11 bit times at the timer 4 baud clock
            ltimer_t *t = &l->tim[4];
            l->uart_tx_data = v;
            l->uart_tx_end = l->ticks + (11u * (t->bkup + 1u) << (7 + t->linking));
            if (!l->uart_tx_end) {
                l->uart_tx_end = 1;
            }
            uart_update(l);
            set_next_event(l);
        } break;
        case 0x8B:  // IODAT: bit 1 cart address data
            l->iodat = v;
            l->cart_addr_data = (v >> 1) & 1;
            break;
        case 0x91: {  // CPUSLEEP: paint sprites now, sleep for their cost
            uint32_t c = lynx_paint_sprites(l);
            l->wake_time = l->ticks + c + 1;
            l->sleeping = 1;
            set_next_event(l);
        } break;
        case 0x92:  // DISPCTL
            l->dma_enable = v & 1;
            l->flip = (v >> 1) & 1;
            break;
        case 0x94: l->disp_addr = (uint16_t)((l->disp_addr & 0xFF00) | v); break;
        case 0x95: l->disp_addr = (uint16_t)((l->disp_addr & 0x00FF) | (v << 8)); break;
        default:
            if (r >= 0xA0 && r < 0xB0) {
                l->palette[r & 15] = (uint16_t)((l->palette[r & 15] & 0xFF0) | (v & 15));
                l->pal_dirty = 1;
            } else if (r >= 0xB0 && r < 0xC0) {
                l->palette[r & 15] = (uint16_t)((l->palette[r & 15] & 0x00F) | ((v & 15) << 4) | ((v & 0xF0) << 4));
                l->pal_dirty = 1;
            }
            break;
    }
}

static uint8_t mikey_read(lynx_t *l, uint16_t addr) {
    uint32_t r = addr & 0xFF;
    if (r < 0x20) {
        int i = r >> 2;
        ltimer_t *t = &l->tim[i];
        switch (r & 3) {
            case 0: return t->bkup;
            case 1: return (uint8_t)(((l->irq_mask >> i) & 1 ? 0x80 : 0) | (t->reload ? 0x10 : 0) |
                                     (t->count ? 0x08 : 0) | t->linking);
            case 2: mikey_update(l); return (uint8_t)t->cur;
            default: return timer_ctlb(t);
        }
    }
    if (r < 0x40) {
        audio_sync(l, l->ticks);
        achan_t *a = &l->aud[(r - 0x20) >> 3];
        switch (r & 7) {
            case 0: return (uint8_t)a->vol;
            case 1: return (uint8_t)(a->ws >> 13);
            case 2: return (uint8_t)a->out;
            case 3: return (uint8_t)a->ws;
            case 4: return a->t.bkup;
            case 5: return (uint8_t)((a->integrate ? 0x20 : 0) | (a->t.reload ? 0x10 : 0) |
                                     (a->t.count ? 0x08 : 0) | ((a->ws & 0x1000) ? 0x80 : 0) | a->t.linking);
            case 6: return (uint8_t)a->t.cur;
            default: return (uint8_t)((a->t.borrow_out ? 1 : 0) | (a->t.borrow_in ? 2 : 0) |
                                      (a->t.last_clock ? 8 : 0) | ((a->ws >> 4) & 0xF0));
        }
    }
    switch (r) {
        case 0x40: case 0x41: case 0x42: case 0x43: return l->atten[r - 0x40];
        case 0x44: return l->pan;
        case 0x50: return l->stereo;
        case 0x80: case 0x81: return l->irq_flags;
        case 0x84: case 0x85: return 0;
        case 0x86: return 0x80;  // AUDIN
        case 0x88: return 0x01;  // MIKEYHREV
        case 0x8B: {  // IODAT
            // Bit 4 (AUDIN / EEPROM data out) reads high as an input: no
            // EEPROM is emulated, and games polling for "write done" would
            // otherwise wait forever.
            uint8_t v = (l->iodir & 0x10) ? (l->iodat & 0x10) : 0x10;
            v |= (l->iodir & 0x08) ? (((l->iodat & 0x08) && l->rest) ? 0 : 0x08) : 0;
            v |= (l->iodir & 0x04) ? (l->iodat & 0x04) : 0;
            v |= (l->iodir & 0x02) ? (l->iodat & 0x02) : 0;
            v |= (l->iodir & 0x01) ? (l->iodat & 0x01) : 0x01;
            return v;
        }
        case 0x8C:  // SERCTL: TXRDY, RXRDY, TXEMPTY
            uart_update(l);
            return (uint8_t)((l->uart_tx_end ? 0 : 0xA0) | (l->uart_rx_ready ? 0x40 : 0));
        case 0x8D:
            l->uart_rx_ready = 0;
            return l->uart_rx_data;
        case 0x94: return (uint8_t)l->disp_addr;
        case 0x95: return (uint8_t)(l->disp_addr >> 8);
        default:
            if (r >= 0xA0 && r < 0xB0) {
                return l->palette[r & 15] & 15;
            }
            if (r >= 0xB0 && r < 0xC0) {
                uint32_t c = l->palette[r & 15];
                return (uint8_t)(((c >> 4) & 15) | ((c >> 4) & 0xF0));
            }
            return 0xFF;
    }
}

// ---------------------------------------------------------------------------
// Suzy

#define SPR_RDWR 3
#define LINE_END 0x80
enum { LINE_ERROR, LINE_ABS_LITERAL, LINE_LITERAL, LINE_PACKED };
enum { SPR_BG_SHADOW, SPR_BG_NOCOLL, SPR_BOUNDARY_SHADOW, SPR_BOUNDARY, SPR_NORMAL, SPR_NOCOLL,
       SPR_XOR_SHADOW, SPR_SHADOW };

#define RAMW(l, a) ((l)->ram[(uint16_t)(a)] | ((l)->ram[(uint16_t)((a) + 1)] << 8))

static uint32_t line_bits(lynx_t *l, uint32_t bits) {
    if (l->line_bits_left <= bits) {
        return 0;
    }
    if (l->line_sr_count < bits) {
        l->line_sr <<= 24;
        l->line_sr |= l->ram[l->tmpadr++] << 16;
        l->line_sr |= l->ram[l->tmpadr++] << 8;
        l->line_sr |= l->ram[l->tmpadr++];
        l->line_sr_count += 24;
        l->spr_cycles += 3 * SPR_RDWR;
    }
    uint32_t v = (l->line_sr >> (l->line_sr_count - bits)) & ((1u << bits) - 1);
    l->line_sr_count -= bits;
    l->line_bits_left -= bits;
    return v;
}

static uint32_t line_init(lynx_t *l, uint32_t voff) {
    l->line_sr = 0;
    l->line_sr_count = 0;
    l->line_repeat = 0;
    l->line_pixel = 0;
    l->line_type = LINE_ERROR;
    l->line_bits_left = 0xFFFF;
    l->tmpadr = l->sprdline;
    uint32_t offset = line_bits(l, 8);
    l->line_bits_left = (offset - 1) * 8;
    if (l->spr_literal) {
        l->line_type = LINE_ABS_LITERAL;
        l->line_repeat = ((offset - 1) * 8) / l->spr_bits;
    }
    if (voff > 101) {
        voff = 0;
    }
    l->line_base = l->vidbas + voff * (LYNX_W / 2);
    l->line_coll = l->collbas + voff * (LYNX_W / 2);
    return offset;
}

static uint32_t line_pixel(lynx_t *l) {
    if (!l->line_repeat) {
        if (l->line_type != LINE_ABS_LITERAL) {
            l->line_type = line_bits(l, 1) ? LINE_LITERAL : LINE_PACKED;
        }
        switch (l->line_type) {
            case LINE_ABS_LITERAL:
                l->line_pixel = LINE_END;
                return LINE_END;
            case LINE_LITERAL:
                l->line_repeat = line_bits(l, 4) + 1;
                break;
            case LINE_PACKED:
                l->line_repeat = line_bits(l, 4);
                if (!l->line_repeat) {
                    l->line_pixel = LINE_END;
                } else {
                    l->line_pixel = l->pen[line_bits(l, l->spr_bits)];
                }
                l->line_repeat++;
                break;
            default:
                return 0;
        }
    }
    if (l->line_pixel != LINE_END) {
        l->line_repeat--;
        switch (l->line_type) {
            case LINE_ABS_LITERAL: {
                uint32_t p = line_bits(l, l->spr_bits);
                if (!l->line_repeat && !p) {
                    l->line_pixel = LINE_END;
                } else {
                    l->line_pixel = l->pen[p];
                }
            } break;
            case LINE_LITERAL:
                l->line_pixel = l->pen[line_bits(l, l->spr_bits)];
                break;
            case LINE_PACKED:
                break;
            default:
                return 0;
        }
    }
    return l->line_pixel;
}

static void write_nib(lynx_t *l, uint32_t base, uint32_t hoff, uint32_t v) {
    uint16_t a = (uint16_t)(base + hoff / 2);
#ifdef LYNX_SPR_WR_HOOK
    LYNX_SPR_WR_HOOK(l, a, v);
#endif
    uint8_t d = l->ram[a];
    l->ram[a] = (hoff & 1) ? (uint8_t)((d & 0xF0) | v) : (uint8_t)((d & 0x0F) | (v << 4));
    l->spr_cycles += 2 * SPR_RDWR;
}

static uint32_t read_nib(lynx_t *l, uint32_t base, uint32_t hoff) {
    uint8_t d = l->ram[(uint16_t)(base + hoff / 2)];
    l->spr_cycles += SPR_RDWR;
    return (hoff & 1) ? (d & 15u) : (uint32_t)(d >> 4);
}

static void collide(lynx_t *l, uint32_t hoff) {
    if (l->coll_off || l->sys_nocollide) {
        return;
    }
    uint32_t c = read_nib(l, l->line_coll, hoff);
    if (c > l->collision) {
        l->collision = c;
    }
    write_nib(l, l->line_coll, hoff, l->coll_num);
}

static void process_pixel(lynx_t *l, uint32_t hoff, uint32_t p) {
    switch (l->spr_type) {
        case SPR_BG_SHADOW:
            write_nib(l, l->line_base, hoff, p);
            if (!l->coll_off && !l->sys_nocollide && p != 0x0E) {
                write_nib(l, l->line_coll, hoff, l->coll_num);
            }
            break;
        case SPR_BG_NOCOLL:
            write_nib(l, l->line_base, hoff, p);
            break;
        case SPR_NOCOLL:
            if (p) {
                write_nib(l, l->line_base, hoff, p);
            }
            break;
        case SPR_BOUNDARY:
            if (p && p != 0x0F) {
                write_nib(l, l->line_base, hoff, p);
            }
            if (p) {
                collide(l, hoff);
            }
            break;
        case SPR_NORMAL:
            if (p) {
                write_nib(l, l->line_base, hoff, p);
                collide(l, hoff);
            }
            break;
        case SPR_BOUNDARY_SHADOW:
            if (p && p != 0x0E && p != 0x0F) {
                write_nib(l, l->line_base, hoff, p);
            }
            if (p && p != 0x0E) {
                collide(l, hoff);
            }
            break;
        case SPR_SHADOW:
            if (p) {
                write_nib(l, l->line_base, hoff, p);
            }
            if (p && p != 0x0E) {
                collide(l, hoff);
            }
            break;
        case SPR_XOR_SHADOW:
            if (p) {
                write_nib(l, l->line_base, hoff, read_nib(l, l->line_base, hoff) ^ p);
            }
            if (p && p != 0x0E) {
                collide(l, hoff);
            }
            break;
    }
}

// Walk the SCB list from SCBNEXT and paint every sprite. Returns the ticks
// the CPU sleeps for. Follows Handy's CSusie::PaintSprites().
static uint32_t lynx_paint_sprites(lynx_t *l) {
    if (!l->busen || !l->sprgo) {
        return 0;
    }
    l->spr_cycles = 0;
    int sprcount = 0;
    for (;;) {
        int everonscreen = 0;
        if (!(l->scbnext & 0xFF00)) {
            l->sprgo = 0;
            break;
        }
        l->tmpadr = l->scbnext;
        l->scbadr = l->scbnext;
        uint8_t d = l->ram[l->tmpadr++];
        l->spr_type = d & 7;
        l->spr_vflip = (d >> 4) & 1;
        l->spr_hflip = (d >> 5) & 1;
        l->spr_bits = (uint8_t)(((d >> 6) & 3) + 1);
        d = l->ram[l->tmpadr++];
        l->spr_left = d & 1;
        l->spr_up = (d >> 1) & 1;
        l->spr_skip = (d >> 2) & 1;
        l->spr_reload_pal = (d >> 3) & 1;
        l->spr_reload_depth = (d >> 4) & 3;
        l->spr_sizing = (d >> 6) & 1;
        l->spr_literal = (d >> 7) & 1;
        d = l->ram[l->tmpadr++];
        l->coll_num = d & 15;
        l->coll_off = (d >> 5) & 1;
        l->scbnext = (uint16_t)RAMW(l, l->tmpadr);
        l->tmpadr += 2;
        l->spr_cycles += 5 * SPR_RDWR;
        l->collision = 0;

        if (!l->spr_skip) {
            l->sprdline = (uint16_t)RAMW(l, l->tmpadr);
            l->tmpadr += 2;
            l->hposstrt = (uint16_t)RAMW(l, l->tmpadr);
            l->tmpadr += 2;
            l->vposstrt = (uint16_t)RAMW(l, l->tmpadr);
            l->tmpadr += 2;
            l->spr_cycles += 6 * SPR_RDWR;
            int enable_stretch = 0, enable_tilt = 0;
            switch (l->spr_reload_depth) {
                case 3:
                    enable_tilt = 1;
                    // fall through
                case 2:
                    enable_stretch = 1;
                    // fall through
                case 1:
                    l->sprhsiz = (uint16_t)RAMW(l, l->tmpadr);
                    l->tmpadr += 2;
                    l->sprvsiz = (uint16_t)RAMW(l, l->tmpadr);
                    l->tmpadr += 2;
                    l->spr_cycles += 4 * SPR_RDWR;
                    if (enable_stretch) {
                        l->stretch = (uint16_t)RAMW(l, l->tmpadr);
                        l->tmpadr += 2;
                        l->spr_cycles += 2 * SPR_RDWR;
                    }
                    if (enable_tilt) {
                        l->tilt = (uint16_t)RAMW(l, l->tmpadr);
                        l->tmpadr += 2;
                        l->spr_cycles += 2 * SPR_RDWR;
                    }
                    break;
                default:
                    break;
            }
            if (!l->spr_reload_pal) {
                for (int i = 0; i < 8; i++) {
                    uint8_t p = l->ram[l->tmpadr++];
                    l->pen[i * 2] = p >> 4;
                    l->pen[i * 2 + 1] = p & 15;
                }
                l->spr_cycles += 8 * SPR_RDWR;
            }

#ifdef LYNX_SPR_HOOK
            LYNX_SPR_HOOK(l);
#endif
            int screen_h_start = (int16_t)l->hoff;
            int screen_h_end = screen_h_start + LYNX_W;
            int screen_v_start = (int16_t)l->voff;
            int screen_v_end = screen_v_start + LYNX_H;
            int world_h_mid = screen_h_start + LYNX_W / 2;
            int world_v_mid = screen_v_start + LYNX_H / 2;
            int quadrant = l->spr_left ? (l->spr_up ? 2 : 3) : (l->spr_up ? 1 : 0);
            int superclip = (int16_t)l->hposstrt < screen_h_start || (int16_t)l->hposstrt >= screen_h_end ||
                            (int16_t)l->vposstrt < screen_v_start || (int16_t)l->vposstrt >= screen_v_end;
            int vquadoff = 0, hquadoff = 0;

            for (int loop = 0; loop < 4; loop++) {
                int sprite_v = l->vposstrt;
                int sprite_h = l->hposstrt;
                int render = 0;
                int hsign = (quadrant == 0 || quadrant == 1) ? 1 : -1;
                int vsign = (quadrant == 0 || quadrant == 3) ? 1 : -1;
                if (l->spr_vflip) {
                    vsign = -vsign;
                }
                if (l->spr_hflip) {
                    hsign = -hsign;
                }
                if (superclip) {
                    static const uint8_t vquadflip[4] = {1, 0, 3, 2};
                    static const uint8_t hquadflip[4] = {3, 2, 1, 0};
                    int modquad = quadrant;
                    if (l->spr_vflip) {
                        modquad = vquadflip[modquad];
                    }
                    if (l->spr_hflip) {
                        modquad = hquadflip[modquad];
                    }
                    switch (modquad) {
                        case 3:
                            render = (sprite_h >= screen_h_start || sprite_h <= world_h_mid) &&
                                     (sprite_v < screen_v_end || sprite_v >= world_v_mid);
                            break;
                        case 2:
                            render = (sprite_h >= screen_h_start || sprite_h <= world_h_mid) &&
                                     (sprite_v >= screen_v_start || sprite_v <= world_v_mid);
                            break;
                        case 1:
                            render = (sprite_h < screen_h_end || sprite_h >= world_h_mid) &&
                                     (sprite_v >= screen_v_start || sprite_v <= world_v_mid);
                            break;
                        default:
                            render = (sprite_h < screen_h_end || sprite_h >= world_h_mid) &&
                                     (sprite_v < screen_v_end || sprite_v >= world_v_mid);
                            break;
                    }
                } else {
                    render = 1;
                }

                if (render) {
                    int voff = (int16_t)l->vposstrt - screen_v_start;
                    l->tiltacum = 0;
                    l->vsizacum = (vsign == 1) ? l->vsizoff : 0;
                    if (loop == 0) {
                        vquadoff = vsign;
                    }
                    if (vsign != vquadoff) {
                        voff += vsign;
                    }
                    for (;;) {
                        l->vsizacum += l->sprvsiz;
                        int pixel_height = l->vsizacum >> 8;
                        l->vsizacum &= 0x00FF;
                        l->sprdoff = (uint16_t)line_init(l, 0);
                        if (l->sprdoff == 1) {
                            l->sprdline += l->sprdoff;
                            break;
                        }
                        if (l->sprdoff == 0) {
                            loop = 4;
                            break;
                        }
                        for (int vloop = 0; vloop < pixel_height; vloop++) {
                            if (vsign == 1 && voff >= LYNX_H) {
                                break;
                            }
                            if (vsign == -1 && voff < 0) {
                                break;
                            }
                            if (voff >= 0 && voff < LYNX_H) {
                                l->hposstrt += (uint16_t)((int16_t)l->tiltacum >> 8);
                                l->tiltacum &= 0x00FF;
                                int hoff = (int)(int16_t)l->hposstrt - screen_h_start;
                                l->hsizacum = l->hsizoff;
                                if (loop == 0) {
                                    hquadoff = hsign;
                                }
                                if (hsign != hquadoff) {
                                    hoff += hsign;
                                }
                                line_init(l, (uint32_t)voff);
                                int onscreen = 0;
                                uint32_t pixel;
                                while ((pixel = line_pixel(l)) != LINE_END) {
                                    l->hsizacum += l->sprhsiz;
                                    int pixel_width = l->hsizacum >> 8;
                                    l->hsizacum &= 0x00FF;
                                    for (int hloop = 0; hloop < pixel_width; hloop++) {
                                        if (hoff >= 0 && hoff < LYNX_W) {
                                            process_pixel(l, (uint32_t)hoff, pixel);
                                            onscreen = 1;
                                            everonscreen = 1;
                                        } else if (onscreen) {
                                            break;
                                        }
                                        hoff += hsign;
                                    }
                                }
                            }
                            voff += vsign;
                            if (enable_stretch) {
                                l->sprhsiz += l->stretch;
                            }
                            if (enable_tilt) {
                                l->tiltacum += l->tilt;
                            }
                        }
                        if (l->sys_vstretch) {
                            l->sprvsiz += (uint16_t)(l->stretch * pixel_height);
                        }
                        l->sprdline += l->sprdoff;
                    }
                } else {
                    for (;;) {
                        l->sprdoff = (uint16_t)line_init(l, 0);
                        l->sprdline += l->sprdoff;
                        if (l->sprdoff == 1) {
                            break;
                        }
                        if (l->sprdoff == 0) {
                            loop = 4;
                            break;
                        }
                    }
                }
                quadrant = (quadrant + 1) & 3;
            }

            if (!l->coll_off && !l->sys_nocollide) {
                switch (l->spr_type) {
                    case SPR_XOR_SHADOW: case SPR_BOUNDARY: case SPR_NORMAL:
                    case SPR_BOUNDARY_SHADOW: case SPR_SHADOW:
                        l->ram[(uint16_t)(l->scbadr + l->colloff)] = (uint8_t)l->collision;
                        break;
                    default:
                        break;
                }
            }
            if (l->everon) {
                uint16_t a = (uint16_t)(l->scbadr + l->colloff);
                l->ram[a] = everonscreen ? (uint8_t)(l->ram[a] & 0x7F) : (uint8_t)(l->ram[a] | 0x80);
            }
        }
        l->spr_painted++;
        if (++sprcount > 4096) {  // looped SCB list
            l->sprgo = 0;
            break;
        }
    }
    return l->spr_cycles;
}

static void math_multiply(lynx_t *l) {
    l->sys_mathbit = 0;
    uint32_t ab = l->math_abcd >> 16, cd = l->math_abcd & 0xFFFF;
    l->math_efgh = ab * cd;
    if (l->sys_signed) {
        l->math_efgh_sign = (int8_t)(l->math_ab_sign + l->math_cd_sign);
        if (!l->math_efgh_sign) {
            l->math_efgh = ~l->math_efgh + 1;
        }
    }
    if (l->sys_accumulate) {
        l->math_jklm += l->math_efgh;
    }
}

static void math_divide(lynx_t *l) {
    l->sys_mathbit = 0;
    if (l->math_np) {
        l->math_abcd = l->math_efgh / l->math_np;
        l->math_jklm = l->math_efgh % l->math_np;
    } else {
        l->math_abcd = 0xFFFFFFFF;
        l->math_jklm = 0;
        l->sys_mathbit = 1;
    }
}

// Byte helpers for the math registers: n is the byte index from the LSB.
#define SETB(w, n, v) ((w) = ((w) & ~(0xFFu << ((n) * 8))) | ((uint32_t)(v) << ((n) * 8)))
#define GETB(w, n) ((uint8_t)((w) >> ((n) * 8)))

static void math_sign_cd(lynx_t *l) {
    if (l->sys_signed) {
        uint32_t cd = l->math_abcd & 0xFFFF;
        if ((cd - 1) & 0x8000) {
            cd = ((cd ^ 0xFFFF) + 1) & 0xFFFF;
            l->math_cd_sign = -1;
            l->math_abcd = (l->math_abcd & 0xFFFF0000) | cd;
        } else {
            l->math_cd_sign = 1;
        }
    }
}

static void suzy_write(lynx_t *l, uint16_t addr, uint8_t v) {
    uint32_t r = addr & 0xFF;
    if (r < 0x30) {
        // 16-bit registers: writing the low byte clears the high byte.
        uint16_t *regs[24] = {
            &l->tmpadr, &l->tiltacum, &l->hoff, &l->voff, &l->vidbas, &l->collbas, &l->vidadr,
            &l->colladr, &l->scbnext, &l->sprdline, &l->hposstrt, &l->vposstrt, &l->sprhsiz,
            &l->sprvsiz, &l->stretch, &l->tilt, &l->sprdoff, &l->sprvpos, &l->colloff, &l->vsizacum,
            &l->hsizoff, &l->vsizoff, &l->scbadr, &l->procadr,
        };
        uint16_t *p = regs[r >> 1];
        *p = (r & 1) ? (uint16_t)((*p & 0x00FF) | (v << 8)) : v;
        return;
    }
    switch (r) {
        case 0x52: SETB(l->math_abcd, 0, v); suzy_write(l, 0xFC53, 0); break;  // D, then C = 0
        case 0x53: SETB(l->math_abcd, 1, v); math_sign_cd(l); break;           // C
        case 0x54: SETB(l->math_abcd, 2, v); SETB(l->math_abcd, 3, 0); break;  // B
        case 0x55:                                                             // A: multiply
            SETB(l->math_abcd, 3, v);
            if (l->sys_signed) {
                uint32_t ab = l->math_abcd >> 16;
                if ((ab - 1) & 0x8000) {
                    ab = ((ab ^ 0xFFFF) + 1) & 0xFFFF;
                    l->math_ab_sign = -1;
                    l->math_abcd = (l->math_abcd & 0xFFFF) | (ab << 16);
                } else {
                    l->math_ab_sign = 1;
                }
            }
            math_multiply(l);
            break;
        case 0x56: SETB(l->math_np, 0, v); SETB(l->math_np, 1, 0); break;  // P
        case 0x57: SETB(l->math_np, 1, v); break;                          // N
        case 0x60: SETB(l->math_efgh, 0, v); SETB(l->math_efgh, 1, 0); break;  // H
        case 0x61: SETB(l->math_efgh, 1, v); break;                            // G
        case 0x62: SETB(l->math_efgh, 2, v); SETB(l->math_efgh, 3, 0); break;  // F
        case 0x63: SETB(l->math_efgh, 3, v); math_divide(l); break;            // E: divide
        case 0x6C: SETB(l->math_jklm, 0, v); SETB(l->math_jklm, 1, 0); l->sys_mathbit = 0; break;  // M
        case 0x6D: SETB(l->math_jklm, 1, v); break;                            // L
        case 0x6E: SETB(l->math_jklm, 2, v); SETB(l->math_jklm, 3, 0); break;  // K
        case 0x6F: SETB(l->math_jklm, 3, v); break;                            // J
        case 0x80:
            l->spr_type = v & 7;
            l->spr_vflip = (v >> 4) & 1;
            l->spr_hflip = (v >> 5) & 1;
            l->spr_bits = (uint8_t)(((v >> 6) & 3) + 1);
            break;
        case 0x81:
            l->spr_left = v & 1;
            l->spr_up = (v >> 1) & 1;
            l->spr_skip = (v >> 2) & 1;
            l->spr_reload_pal = (v >> 3) & 1;
            l->spr_reload_depth = (v >> 4) & 3;
            l->spr_sizing = (v >> 6) & 1;
            l->spr_literal = (v >> 7) & 1;
            break;
        case 0x82: l->coll_num = v & 15; l->coll_off = (v >> 5) & 1; break;
        case 0x83: l->sprinit = v; break;
        case 0x90: l->busen = v & 1; break;
        case 0x91: l->sprgo = v & 1; l->everon = (v >> 2) & 1; break;
        case 0x92:
            l->sys_stop = (v >> 1) & 1;
            if (v & 4) {
                l->sys_unsafe = 0;
            }
            l->sys_lefthand = (v >> 3) & 1;
            l->sys_vstretch = (v >> 4) & 1;
            l->sys_nocollide = (v >> 5) & 1;
            l->sys_accumulate = (v >> 6) & 1;
            l->sys_signed = (v >> 7) & 1;
            break;
        case 0xB2: case 0xB3:  // RCART writes: no writable carts
            if (!l->cart_strobe) {
                l->cart_counter = (l->cart_counter + 1) & 0x7FF;
            }
            break;
        default:
            break;
    }
}

static uint8_t suzy_read(lynx_t *l, uint16_t addr) {
    uint32_t r = addr & 0xFF;
    if (r < 0x30) {
        const uint16_t regs[24] = {
            l->tmpadr, l->tiltacum, l->hoff, l->voff, l->vidbas, l->collbas, l->vidadr,
            l->colladr, l->scbnext, l->sprdline, l->hposstrt, l->vposstrt, l->sprhsiz,
            l->sprvsiz, l->stretch, l->tilt, l->sprdoff, l->sprvpos, l->colloff, l->vsizacum,
            l->hsizoff, l->vsizoff, l->scbadr, l->procadr,
        };
        uint16_t w = regs[r >> 1];
        return (r & 1) ? (uint8_t)(w >> 8) : (uint8_t)w;
    }
    switch (r) {
        case 0x52: return GETB(l->math_abcd, 0);
        case 0x53: return GETB(l->math_abcd, 1);
        case 0x54: return GETB(l->math_abcd, 2);
        case 0x55: return GETB(l->math_abcd, 3);
        case 0x56: return GETB(l->math_np, 0);
        case 0x57: return GETB(l->math_np, 1);
        case 0x60: return GETB(l->math_efgh, 0);
        case 0x61: return GETB(l->math_efgh, 1);
        case 0x62: return GETB(l->math_efgh, 2);
        case 0x63: return GETB(l->math_efgh, 3);
        case 0x6C: return GETB(l->math_jklm, 0);
        case 0x6D: return GETB(l->math_jklm, 1);
        case 0x6E: return GETB(l->math_jklm, 2);
        case 0x6F: return GETB(l->math_jklm, 3);
        case 0x88: return 0x01;  // SUZYHREV
        case 0x92:
            return (uint8_t)((l->sleeping ? 1 : 0) | (l->sys_stop ? 2 : 0) | (l->sys_unsafe ? 4 : 0) |
                             (l->sys_lefthand ? 8 : 0) | (l->sys_vstretch ? 0x10 : 0) |
                             (l->sys_lastcarry ? 0x20 : 0) | (l->sys_mathbit ? 0x40 : 0));
        case 0xB0: {  // JOYSTICK
            uint8_t j = (uint8_t)l->joy;
            if (l->sys_lefthand) {  // handheld flipped: swap up/down and left/right
                j = (uint8_t)((j & 0x0F) | ((j & LYNX_UP) >> 1) | ((j & LYNX_DOWN) << 1) |
                              ((j & LYNX_LEFT) >> 1) | ((j & LYNX_RIGHT) << 1));
            }
            return j;
        }
        case 0xB1: return (uint8_t)(l->joy >> 8);  // SWITCHES: bit 0 pause
        case 0xB2: return cart_read(l);
        case 0xB3: return 0xFF;  // bank 1 absent
        default: return 0xFF;
    }
}

// ---------------------------------------------------------------------------
// CPU bus

static uint8_t io_read(lynx_t *l, uint16_t a) {
    if (a < 0xFD00) {
        return (l->mapctl & 1) ? l->ram[a] : suzy_read(l, a);
    }
    if (a < 0xFE00) {
        return (l->mapctl & 2) ? l->ram[a] : mikey_read(l, a);
    }
    if (a < 0xFFF8) {
        return (l->mapctl & 4) ? l->ram[a] : l->rom[a - 0xFE00];
    }
    if (a == 0xFFF8) {
        return l->ram[a];
    }
    if (a == 0xFFF9) {
        return l->mapctl;
    }
    return (l->mapctl & 8) ? l->ram[a] : l->rom[a - 0xFE00];
}

static void io_write(lynx_t *l, uint16_t a, uint8_t v) {
    if (a < 0xFD00) {
        if (l->mapctl & 1) {
            l->ram[a] = v;
        } else {
            suzy_write(l, a, v);
        }
    } else if (a < 0xFE00) {
        if (l->mapctl & 2) {
            l->ram[a] = v;
        } else {
            mikey_write(l, a, v);
        }
    } else if (a == 0xFFF9) {
        l->mapctl = v;
    } else {
        l->ram[a] = v;  // writes under the ROM and vectors go to RAM
    }
}

static inline uint8_t cpu_rd(lynx_t *l, uint16_t a) {
    return a < 0xFC00 ? l->ram[a] : io_read(l, a);
}

// ---------------------------------------------------------------------------
// Boot

static void mikey_preset(lynx_t *l) {
    // What the boot ROM leaves running: line timer 159 us, 105 lines a
    // frame (59.9 Hz), display DMA on. Some homebrew relies on it.
    l->tim[0].bkup = 0x9E;
    l->tim[0].reload = 1;
    l->tim[0].count = 1;
    l->tim[2].bkup = 0x68;
    l->tim[2].reload = 1;
    l->tim[2].count = 1;
    l->tim[2].linking = 7;
    l->dma_enable = 1;
    l->flip = 0;
}

static void hle_boot(lynx_t *l) {
    memset(l->ram, 0, sizeof(l->ram));
    l->ram[5] = 0x00;
    l->ram[6] = 0x02;
    l->cart_shifter = 0;
    l->cart_counter = 0;
    l->boot_addr = 0x200;
    l->boot_blocks = hle_load_frame(l, 0x200);
    l->PC = 0x200;
}

// Called when the CPU is about to execute from the ROM area with the ROM
// mapped and no real boot ROM. Entry points as in Handy's fake BIOS.
static void hle_call(lynx_t *l) {
    switch (l->PC) {
        case 0xFE00: {  // select cart block A, then RTS
            l->cart_shifter = l->A;
            l->cart_counter = 0;
            uint8_t lo = l->ram[0x100 | (uint8_t)(l->S + 1)];
            uint8_t hi = l->ram[0x100 | (uint8_t)(l->S + 2)];
            l->S += 2;
            l->PC = (uint16_t)((lo | (hi << 8)) + 1);
        } break;
        case 0xFE19:
        case 0xFF80:
            hle_boot(l);
            break;
        case 0xFE4A:
            hle_load_frame(l, (uint16_t)(l->ram[5] | (l->ram[6] << 8)));
            l->PC = 0x200;
            break;
        default:
            break;
    }
}

// Load a .lnx, raw cart or BS93 homebrew image. boot_rom is a 512 byte
// lynxboot.img or NULL for the HLE boot. Returns 0 or an error code.
static int lynx_init(lynx_t *l, const uint8_t *data, uint32_t len, const uint8_t *boot_rom) {
    memset(l, 0, sizeof(*l));
    l->S = 0xFF;
    l->I_ = 1;
    l->Z_ = 0;
    l->mapctl = 0;
    l->hsizoff = 0x007F;
    l->vsizoff = 0x007F;
    l->math_abcd = l->math_efgh = l->math_jklm = 0xFFFFFFFF;
    l->math_np = 0xFFFF;
    l->math_ab_sign = l->math_cd_sign = l->math_efgh_sign = 1;
    for (int i = 0; i < 16; i++) {
        l->pen[i] = (uint8_t)i;
    }
    l->atten[0] = l->atten[1] = l->atten[2] = l->atten[3] = 0xFF;
    l->pal_dirty = 1;
    l->next_sample = LYNX_SAMPLE_TICKS;
    l->timer_event = 0;
    l->next_event = 0;

    // Fake ROM: filler, vectors (NMI and IRQ to an RTI, reset to $FF80).
    memset(l->rom, 0x88, sizeof(l->rom));
    l->rom[0x190] = 0x40;  // $FF90: RTI
    l->rom[0x1FA] = 0x90; l->rom[0x1FB] = 0xFF;
    l->rom[0x1FC] = 0x80; l->rom[0x1FD] = 0xFF;
    l->rom[0x1FE] = 0x90; l->rom[0x1FF] = 0xFF;

    if (len >= 10 && data[6] == 'B' && data[7] == 'S' && data[8] == '9' && data[9] == '3') {
        uint32_t load = ((uint32_t)data[2] << 8 | data[3]) - 10;
        uint32_t n = len;
        if (load + n > 0x10000) {
            n = 0x10000 - load;
        }
        memcpy(l->ram + load, data, n);
        l->boot = BOOT_BS93;
        l->boot_addr = (uint16_t)load;
        l->boot_blocks = n;
        l->PC = (uint16_t)load;
        l->hle = 1;
        mikey_preset(l);
        return 0;
    }

    uint32_t page = 0;
    if (len >= 64 && data[0] == 'L' && data[1] == 'Y' && data[2] == 'N' && data[3] == 'X') {
        page = data[4] | (data[5] << 8);
        l->rotation = data[58];
        l->cart = data + 64;
        l->cart_len = len - 64;
    } else {
        l->cart = data;  // headerless: guess the page size from the length
        l->cart_len = len;
        page = len >> 8;
    }
    switch (page) {
        case 0x100: l->bank0_shift = 8; break;
        case 0x200: l->bank0_shift = 9; break;
        case 0x400: l->bank0_shift = 10; break;
        case 0x800: l->bank0_shift = 11; break;
        default:
            l->bank0_shift = page <= 0x100 ? 8 : page <= 0x200 ? 9 : page <= 0x400 ? 10 : 11;
            break;
    }
    l->bank0_count_mask = (1u << l->bank0_shift) - 1;
    l->bank0_mask = (1u << (l->bank0_shift + 8)) - 1;
    if (l->cart_len < 52) {
        return 1;
    }
    if (boot_rom) {
        memcpy(l->rom, boot_rom, sizeof(l->rom));
        l->boot = BOOT_ROM;
        l->PC = (uint16_t)(l->rom[0x1FC] | (l->rom[0x1FD] << 8));
        return 0;
    }
    l->boot = BOOT_HLE;
    l->hle = 1;
    mikey_preset(l);
    hle_boot(l);
    return 0;
}

// ---------------------------------------------------------------------------
// Main loop

static void lynx_fix_wrap(lynx_t *l) {
    const uint32_t d = 0x80000000u;
    l->ticks -= d;
    l->next_event -= d;
    l->timer_event -= d;
    l->next_sample -= d;
    if (l->wake_time) {
        l->wake_time -= d;
    }
    for (int i = 0; i < 8; i++) {
        l->tim[i].last -= d;
    }
    for (int i = 0; i < 4; i++) {
        l->aud[i].t.last -= d;
        l->aud[i].acc_t -= d;
    }
}

// Run until the given number of end-of-frame events, or max_ticks pass
// (so a game that stops its display timers still returns).
static void lynx_run(lynx_t *l, uint32_t frames, uint32_t max_ticks) {
    uint8_t A = l->A, X = l->X, Y = l->Y, S = l->S;
    uint16_t PC = l->PC;
    unsigned C_ = l->C_, V_ = l->V_, D_ = l->D_, I_ = l->I_, N_ = l->N_, Z_ = l->Z_;
    uint32_t target = l->frames + frames;
    uint32_t end = l->ticks + max_ticks;
    l->samples = 0;
    // Hot state in locals: RAM stores through a uint8_t pointer may alias
    // anything in *l, so fields would be reloaded after every write.
    uint8_t *const ram = l->ram;
    uint32_t ticks = l->ticks, next_event = l->next_event;
    int irq = l->irq_flags != 0;
    int halted = l->sleeping || l->wai;

// Registers out to the struct and back, around anything that may read or
// change them (HLE calls).
#define SYNC_OUT() do { l->A = A; l->X = X; l->Y = Y; l->S = S; l->PC = PC; \
        l->C_ = C_; l->V_ = V_; l->D_ = D_; l->I_ = I_; l->N_ = N_; l->Z_ = Z_; } while (0)
#define SYNC_IN() do { A = l->A; X = l->X; Y = l->Y; S = l->S; PC = l->PC; } while (0)
// After anything that may run Mikey or Suzy.
#define STATE_IN() do { ticks = l->ticks; next_event = l->next_event; irq = l->irq_flags != 0; \
        halted = l->sleeping || l->wai; } while (0)

    for (;;) {
        if (halted && (int32_t)(ticks - next_event) < 0) {
            ticks = next_event;
        }
        if ((int32_t)(ticks - next_event) >= 0) {
            l->ticks = ticks;
            if (ticks > 0xF0000000u) {
                lynx_fix_wrap(l);
                end -= 0x80000000u;
            }
            while ((int32_t)(l->ticks - l->next_sample) >= 0) {
                audio_sample(l);
            }
            if ((int32_t)(l->ticks - l->timer_event) >= 0 ||
                (l->wake_time && (int32_t)(l->ticks - l->wake_time) >= 0)) {
                mikey_update(l);
            }
            set_next_event(l);
            STATE_IN();
            if (l->frames == target || (int32_t)(ticks - end) >= 0 || l->stopped) {
                break;
            }
        }
        if (irq) {
            if (l->wai) {
                l->wai = 0;
                halted = l->sleeping;
            }
            if (!I_) {
                uint8_t p = (uint8_t)((N_ & 0x80) | (V_ << 6) | 0x20 | (D_ << 3) | (I_ << 2) | (Z_ ? 0 : 2) | C_);
                ram[0x100 | S] = (uint8_t)(PC >> 8); S--;
                ram[0x100 | S] = (uint8_t)PC; S--;
                ram[0x100 | S] = p; S--;
                I_ = 1;
                D_ = 0;
                PC = (uint16_t)(cpu_rd(l, 0xFFFE) | (cpu_rd(l, 0xFFFF) << 8));
                l->sleeping = 0;
                halted = 0;
                ticks += 7 * 5 - 4;
            }
        }
        if (halted) {
            continue;
        }
#ifdef LYNX_HOOK
        LYNX_HOOK(l, PC);
#endif
        if (PC >= 0xFE00 && l->hle && !(l->mapctl & 4) &&
            (PC == 0xFE00 || PC == 0xFE19 || PC == 0xFE4A || PC == 0xFF80)) {
            SYNC_OUT();
            hle_call(l);
            SYNC_IN();
            continue;
        }
#ifndef LYNX_WR_HOOK
#define LYNX_WR_HOOK(a, v, pc)
#endif
        unsigned cyc = 0;
#define RD(a) __extension__({ uint16_t a_ = (uint16_t)(a); uint8_t v_; cyc++; \
        if (a_ < 0xFC00) { v_ = ram[a_]; } else { l->ticks = ticks + cyc * 5; cyc = 0; \
            v_ = io_read(l, a_); STATE_IN(); } v_; })
#define WR(a, v) do { uint16_t a_ = (uint16_t)(a); uint8_t v_ = (uint8_t)(v); cyc++; \
        LYNX_WR_HOOK(a_, v_, PC); \
        if (a_ < 0xFC00) { ram[a_] = v_; } else { l->ticks = ticks + cyc * 5; cyc = 0; \
            io_write(l, a_, v_); STATE_IN(); } } while (0)
#define TICK() (cyc++)
#define WAIT() (l->wai = 1, halted = 1)
#define STOP() (l->stopped = 1, l->err = 3, next_event = ticks)
#include "cpu65c02.h"
#undef RD
#undef WR
#undef TICK
#undef WAIT
#undef STOP
        ticks += cyc * 5 - 4;
    }
    l->ticks = ticks;
    SYNC_OUT();
#undef SYNC_OUT
#undef SYNC_IN
#undef STATE_IN
}
