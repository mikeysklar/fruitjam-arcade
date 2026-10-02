// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Small NES core for microcontrollers: 6502 CPU, scanline PPU, APU, and
// mappers 0 (NROM), 1 (MMC1), 2 (UxROM), 3 (CNROM), 4 (MMC3), 7 (AxROM),
// 66 (GxROM). NTSC timing only.
//
// Design, chosen for speed over accuracy:
// - The CPU runs whole instructions, counting cycles. Memory-mapped I/O sees
//   the cycle count at the end of the instruction.
// - The PPU draws a whole scanline at dot 0 of that line from the current
//   scroll registers, then the CPU runs through the line. Sprite 0 hit is
//   found while drawing and its flag is raised when the CPU reaches that dot.
//   Mid-line writes to scroll, palette or banks take effect on the next line.
// - MMC3 IRQ counter is clocked once per rendered line at dot 260.
// - The APU is caught up lazily (on register access and each scanline) and
//   point-samples each channel at the output rate.
//
// The core allocates nothing and uses no writable static data. All state is in
// nes_t, which the caller owns. The ROM image is only read.
//
// Required from the includer: memset, memcpy, and a callback
//   static void nes_draw_line(nes_t *n, int line, const uint8_t *slots,
//                             const uint8_t *colors);
// which receives, per visible line, 256 palette slots (0..31) and the NES
// color (0..63) of each slot: pixel x is colors[slots[x]].

#include "nes_tables.h"

#define NES_W 256
#define NES_H 240
#define NES_AUDIO_RATE 22050
// Samples per emulated frame. 367 at 22050 Hz is 60.08 fps, 0.03% slower
// than the NTSC NES (60.0988); the pitch error is inaudible.
#define NES_AUDIO_SAMPLES 367
#define NES_FRAME_DOTS 89342  // 262 lines x 341 dots, even frame

// Joypad bits, NES shift order.
#define NES_A 0x01
#define NES_B 0x02
#define NES_SELECT 0x04
#define NES_START 0x08
#define NES_UP 0x10
#define NES_DOWN 0x20
#define NES_LEFT 0x40
#define NES_RIGHT 0x80

enum {
    NES_OK = 0,
    NES_ERR_HEADER = 1,     // not an iNES file or truncated
    NES_ERR_MAPPER = 2,     // mapper not supported
};

typedef struct {
    uint8_t enabled, duty, step, length, halt, constvol, vol;
    uint8_t env_start, env_div, env_decay;
    uint8_t sw_en, sw_period, sw_neg, sw_shift, sw_reload, sw_div;
    uint16_t timer;
    int32_t acc;
} apu_pulse_t;

typedef struct {
    // CPU
    uint16_t pc;
    uint8_t a, x, y, s;
    uint8_t fc, fv, fi, fd, nr, zr;  // flags; Z is (zr == 0), N is nr bit 7
    uint8_t nmi;                      // NMI pending
    uint8_t irq;                      // IRQ sources: 1 mapper, 2 APU frame, 4 DMC
    uint8_t err;                      // 1 = CPU jammed (illegal opcode)
    uint16_t err_addr;
    int32_t cyc;                      // CPU cycles; cycle c is frame dot 3c - phase
    int32_t phase;                    // 0..2, see cyc
    uint32_t frame;
    uint32_t instrs;                  // instructions executed (bench only)

    // PPU
    uint8_t ctrl, mask, status, oamaddr;
    uint8_t fine_x, w, readbuf, latch;
    uint16_t v, t;
    int16_t s0_dot;                   // dot of this line's sprite 0 hit, -1 none

    // Cartridge
    const uint8_t *prg_rom;
    const uint8_t *chr_rom;
    uint32_t prg_size, chr_size;      // bytes
    uint16_t mapper;
    uint8_t battery, four_screen, chr_is_ram, mirror;
    uint8_t *prg_ram;                 // 8 KB at $6000, internal or caller's
    const uint8_t *prg[4];            // 8 KB banks at $8000, $A000, $C000, $E000
    uint8_t *chr[8];                  // 1 KB pattern banks (writable when CHR RAM)
    uint8_t *nt[4];                   // 1 KB nametables

    // Mapper registers
    uint8_t mmc1_shift, mmc1_count, mmc1_ctrl, mmc1_chr0, mmc1_chr1, mmc1_prg;
    uint8_t mmc3_sel, mmc3_reg[8], mmc3_latch, mmc3_counter, mmc3_reload, mmc3_irq_en;
    uint8_t bank_reg;

    // Input
    uint8_t pad, pad_shift, strobe;

    // APU
    apu_pulse_t pulse[2];
    uint8_t tri_enabled, tri_length, tri_halt, tri_linear, tri_linear_reload, tri_reload_flag, tri_step;
    uint16_t tri_timer;
    int32_t tri_acc;
    uint8_t noise_enabled, noise_length, noise_halt, noise_constvol, noise_vol, noise_mode;
    uint8_t noise_env_start, noise_env_div, noise_env_decay, noise_period_ix;
    uint16_t noise_lfsr;
    int32_t noise_acc;
    uint8_t dmc_irq_en, dmc_loop, dmc_rate_ix, dmc_level;
    uint16_t dmc_addr_start, dmc_len_start, dmc_addr, dmc_left;
    uint8_t dmc_shift, dmc_bits, dmc_silence, dmc_buf, dmc_buf_full;
    int32_t dmc_acc;
    uint8_t fc_mode, fc_inhibit, fc_step;
    int32_t fc_next;                  // dot of next frame counter step
    int32_t apu_dot;                  // dot the channels are caught up to
    uint32_t sample_ix;               // samples made this frame
    int32_t dc;                       // DC tracker for the high-pass
    int16_t audio[NES_AUDIO_SAMPLES];

    // Memory
    uint8_t ram[0x800];
    uint8_t oam[256];
    uint8_t pal[32];
    uint8_t vram[0x1000];             // 2 KB, or 4 KB for four-screen carts
    uint8_t chr_ram[0x2000];
    uint8_t prg_ram_int[0x2000];
    uint8_t bg[NES_W + 16];           // background pixel values for one line
    uint8_t sp[NES_W];                // pixels owned by a sprite this line (== sp_tag)
    uint8_t sp_tag;
    uint8_t line[NES_W];              // palette slots (0..31), rendering off
    uint8_t colors[32];               // NES color (0..63) of each palette slot
    uint8_t colors_dirty;             // palette or greyscale changed since built
    uint32_t colors_ver;              // bumped whenever colors[] changes
} nes_t;

static void nes_draw_line(nes_t *n, int line, const uint8_t *slots, const uint8_t *colors);

// ---------------------------------------------------------------------------
// Mappers

static void set_prg8(nes_t *n, int slot, int bank) {
    uint32_t count = n->prg_size >> 13;
    if (bank < 0) {
        bank += count;
    }
    n->prg[slot] = n->prg_rom + (((uint32_t)bank % count) << 13);
}

static void set_prg16(nes_t *n, int slot, int bank) {
    uint32_t count = n->prg_size >> 14;
    if (bank < 0) {
        bank += count;
    }
    bank = (uint32_t)bank % count;
    set_prg8(n, slot, bank * 2);
    set_prg8(n, slot + 1, bank * 2 + 1);
}

static void set_prg32(nes_t *n, int bank) {
    set_prg16(n, 0, bank * 2);
    set_prg16(n, 2, bank * 2 + 1);
}

static void set_chr1(nes_t *n, int slot, int bank) {
    uint32_t count = n->chr_size >> 10;
    uint8_t *base = n->chr_is_ram ? n->chr_ram : (uint8_t *)n->chr_rom;
    n->chr[slot] = base + (((uint32_t)bank % count) << 10);
}

static void set_chr4(nes_t *n, int slot, int bank) {
    for (int i = 0; i < 4; i++) {
        set_chr1(n, slot + i, bank * 4 + i);
    }
}

static void set_chr8(nes_t *n, int bank) {
    set_chr4(n, 0, bank * 2);
    set_chr4(n, 4, bank * 2 + 1);
}

// 0 one-screen low, 1 one-screen high, 2 vertical, 3 horizontal
static void set_mirror(nes_t *n, int m) {
    if (n->four_screen) {
        for (int i = 0; i < 4; i++) {
            n->nt[i] = n->vram + i * 0x400;
        }
        return;
    }
    n->mirror = m;
    static const uint8_t map[4][4] = {{0, 0, 0, 0}, {1, 1, 1, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
    for (int i = 0; i < 4; i++) {
        n->nt[i] = n->vram + map[m][i] * 0x400;
    }
}

static void mmc1_update(nes_t *n) {
    set_mirror(n, n->mmc1_ctrl & 3);
    // SUROM (512 KB): CHR register bit 4 picks the 256 KB PRG half.
    int outer = n->prg_size > 0x40000 ? (n->mmc1_chr0 & 0x10) : 0;
    int p = n->mmc1_prg & 0x0F;
    switch ((n->mmc1_ctrl >> 2) & 3) {
        case 0:
        case 1:
            set_prg16(n, 0, outer | (p & 0x0E));
            set_prg16(n, 2, outer | (p | 1));
            break;
        case 2:
            set_prg16(n, 0, outer);
            set_prg16(n, 2, outer | p);
            break;
        default:
            set_prg16(n, 0, outer | p);
            set_prg16(n, 2, outer | 0x0F);
            break;
    }
    if (n->mmc1_ctrl & 0x10) {
        set_chr4(n, 0, n->mmc1_chr0);
        set_chr4(n, 4, n->mmc1_chr1);
    } else {
        set_chr8(n, n->mmc1_chr0 >> 1);
    }
}

static void mmc3_update(nes_t *n) {
    const uint8_t *r = n->mmc3_reg;
    if (n->mmc3_sel & 0x40) {
        set_prg8(n, 0, -2);
        set_prg8(n, 2, r[6]);
    } else {
        set_prg8(n, 0, r[6]);
        set_prg8(n, 2, -2);
    }
    set_prg8(n, 1, r[7]);
    set_prg8(n, 3, -1);
    int inv = (n->mmc3_sel & 0x80) ? 4 : 0;
    set_chr1(n, 0 ^ inv, r[0] & 0xFE);
    set_chr1(n, 1 ^ inv, r[0] | 1);
    set_chr1(n, 2 ^ inv, r[1] & 0xFE);
    set_chr1(n, 3 ^ inv, r[1] | 1);
    set_chr1(n, 4 ^ inv, r[2]);
    set_chr1(n, 5 ^ inv, r[3]);
    set_chr1(n, 6 ^ inv, r[4]);
    set_chr1(n, 7 ^ inv, r[5]);
}

static void mapper_write(nes_t *n, uint16_t addr, uint8_t val) {
    switch (n->mapper) {
        case 1:
            if (val & 0x80) {
                n->mmc1_shift = 0;
                n->mmc1_count = 0;
                n->mmc1_ctrl |= 0x0C;
                mmc1_update(n);
                return;
            }
            n->mmc1_shift |= (val & 1) << n->mmc1_count;
            if (++n->mmc1_count < 5) {
                return;
            }
            switch ((addr >> 13) & 3) {
                case 0: n->mmc1_ctrl = n->mmc1_shift; break;
                case 1: n->mmc1_chr0 = n->mmc1_shift; break;
                case 2: n->mmc1_chr1 = n->mmc1_shift; break;
                default: n->mmc1_prg = n->mmc1_shift; break;
            }
            n->mmc1_shift = 0;
            n->mmc1_count = 0;
            mmc1_update(n);
            break;
        case 2:
            set_prg16(n, 0, val);
            break;
        case 3:
            set_chr8(n, val);
            break;
        case 4:
            switch (((addr >> 12) & 6) | (addr & 1)) {
                case 0: n->mmc3_sel = val; mmc3_update(n); break;
                case 1: n->mmc3_reg[n->mmc3_sel & 7] = val; mmc3_update(n); break;
                case 2: set_mirror(n, (val & 1) ? 3 : 2); break;
                case 3: break;  // PRG RAM protect, ignored
                case 4: n->mmc3_latch = val; break;
                case 5: n->mmc3_counter = 0; n->mmc3_reload = 1; break;
                case 6: n->mmc3_irq_en = 0; n->irq &= ~1; break;
                default: n->mmc3_irq_en = 1; break;
            }
            break;
        case 7:
            set_prg32(n, val & 7);
            set_mirror(n, (val >> 4) & 1);
            break;
        case 66:
            set_prg32(n, (val >> 4) & 3);
            set_chr8(n, val & 3);
            break;
        default:
            break;
    }
}

// Called once per rendered line at dot 260 (MMC3 A12 rise, standard layout).
static void mapper_scanline(nes_t *n) {
    if (n->mapper != 4) {
        return;
    }
    if (n->mmc3_counter == 0 || n->mmc3_reload) {
        n->mmc3_counter = n->mmc3_latch;
        n->mmc3_reload = 0;
    } else {
        n->mmc3_counter--;
    }
    if (n->mmc3_counter == 0 && n->mmc3_irq_en) {
        n->irq |= 1;
    }
}

static void mapper_reset(nes_t *n) {
    set_prg32(n, 0);
    set_prg16(n, 2, -1);
    set_chr8(n, 0);
    switch (n->mapper) {
        case 1:
            n->mmc1_ctrl = 0x0C;
            mmc1_update(n);
            break;
        case 4:
            n->mmc3_reg[6] = 0;
            n->mmc3_reg[7] = 1;
            mmc3_update(n);
            break;
        case 7:
            set_prg32(n, 0);
            set_mirror(n, 0);
            break;
        case 66:
            set_prg32(n, 0);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// APU

static const uint8_t apu_length_table[32] = {
    10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30,
};
static const uint8_t apu_duty[4] = {0x02, 0x06, 0x1E, 0xF9};  // bit per step
static const uint8_t apu_tri[32] = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};
static const uint16_t apu_noise_period[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068,
};
static const uint16_t apu_dmc_rate[16] = {
    428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54,
};
// Frame counter steps in dots (CPU cycles x 3), 4-step then 5-step mode.
static const int32_t apu_fc4[4] = {22372, 44740, 67114, 89488};
static const int32_t apu_fc5[5] = {22372, 44740, 67114, 89488, 111844};

static uint8_t cpu_peek(nes_t *n, uint16_t addr);

// Current frame dot of the CPU.
static inline int32_t nes_dot(const nes_t *n) {
    return n->cyc * 3 - n->phase;
}

static uint16_t pulse_target(const apu_pulse_t *p, int ones) {
    uint16_t delta = p->timer >> p->sw_shift;
    if (p->sw_neg) {
        return p->timer - delta - ones;
    }
    return p->timer + delta;
}

static int pulse_muted(const apu_pulse_t *p) {
    return p->timer < 8 || (!p->sw_neg && pulse_target(p, 0) > 0x7FF);
}

static void apu_quarter(nes_t *n) {
    for (int i = 0; i < 2; i++) {
        apu_pulse_t *p = &n->pulse[i];
        if (p->env_start) {
            p->env_start = 0;
            p->env_decay = 15;
            p->env_div = p->vol;
        } else if (p->env_div) {
            p->env_div--;
        } else {
            p->env_div = p->vol;
            if (p->env_decay) {
                p->env_decay--;
            } else if (p->halt) {
                p->env_decay = 15;
            }
        }
    }
    if (n->noise_env_start) {
        n->noise_env_start = 0;
        n->noise_env_decay = 15;
        n->noise_env_div = n->noise_vol;
    } else if (n->noise_env_div) {
        n->noise_env_div--;
    } else {
        n->noise_env_div = n->noise_vol;
        if (n->noise_env_decay) {
            n->noise_env_decay--;
        } else if (n->noise_halt) {
            n->noise_env_decay = 15;
        }
    }
    if (n->tri_reload_flag) {
        n->tri_linear = n->tri_linear_reload;
    } else if (n->tri_linear) {
        n->tri_linear--;
    }
    if (!n->tri_halt) {
        n->tri_reload_flag = 0;
    }
}

static void apu_half(nes_t *n) {
    for (int i = 0; i < 2; i++) {
        apu_pulse_t *p = &n->pulse[i];
        if (p->length && !p->halt) {
            p->length--;
        }
        if (p->sw_div == 0 && p->sw_en && p->sw_shift && !pulse_muted(p)) {
            p->timer = pulse_target(p, i == 0);
        }
        if (p->sw_div == 0 || p->sw_reload) {
            p->sw_div = p->sw_period;
            p->sw_reload = 0;
        } else {
            p->sw_div--;
        }
    }
    if (n->tri_length && !n->tri_halt) {
        n->tri_length--;
    }
    if (n->noise_length && !n->noise_halt) {
        n->noise_length--;
    }
}

static void apu_frame_step(nes_t *n) {
    int st = n->fc_step;
    if (n->fc_mode == 0) {
        apu_quarter(n);
        if (st == 1 || st == 3) {
            apu_half(n);
        }
        if (st == 3 && !n->fc_inhibit) {
            n->irq |= 2;
        }
        n->fc_step = (st + 1) & 3;
        n->fc_next += st == 3 ? apu_fc4[0] + (89490 - apu_fc4[3]) : apu_fc4[st + 1] - apu_fc4[st];
    } else {
        if (st != 3) {
            apu_quarter(n);
        }
        if (st == 1 || st == 4) {
            apu_half(n);
        }
        n->fc_step = st == 4 ? 0 : st + 1;
        n->fc_next += st == 4 ? apu_fc5[0] + (111846 - apu_fc5[4]) : apu_fc5[st + 1] - apu_fc5[st];
    }
}

static int pulse_sample(apu_pulse_t *p, int32_t dt) {
    if (!p->length || pulse_muted(p)) {
        return 0;
    }
    int32_t per = (p->timer + 1) * 6;
    p->acc += dt;
    if (p->acc >= per) {
        int32_t k = p->acc / per;
        p->acc -= k * per;
        p->step = (p->step + k) & 7;
    }
    if (!((apu_duty[p->duty] >> p->step) & 1)) {
        return 0;
    }
    return p->constvol ? p->vol : p->env_decay;
}

static void dmc_clock(nes_t *n) {
    if (!n->dmc_silence) {
        if (n->dmc_shift & 1) {
            if (n->dmc_level <= 125) {
                n->dmc_level += 2;
            }
        } else if (n->dmc_level >= 2) {
            n->dmc_level -= 2;
        }
    }
    n->dmc_shift >>= 1;
    if (--n->dmc_bits == 0) {
        n->dmc_bits = 8;
        if (n->dmc_buf_full) {
            n->dmc_shift = n->dmc_buf;
            n->dmc_buf_full = 0;
            n->dmc_silence = 0;
        } else {
            n->dmc_silence = 1;
        }
    }
    if (!n->dmc_buf_full && n->dmc_left) {
        n->dmc_buf = cpu_peek(n, n->dmc_addr);
        n->dmc_buf_full = 1;
        n->dmc_addr = n->dmc_addr == 0xFFFF ? 0x8000 : n->dmc_addr + 1;
        if (--n->dmc_left == 0) {
            if (n->dmc_loop) {
                n->dmc_addr = n->dmc_addr_start;
                n->dmc_left = n->dmc_len_start;
            } else if (n->dmc_irq_en) {
                n->irq |= 4;
            }
        }
    }
}

// One output sample at dot `now`, dt dots after the previous one.
static void apu_sample(nes_t *n, int32_t dt) {
    int p = pulse_sample(&n->pulse[0], dt) + pulse_sample(&n->pulse[1], dt);
    if (n->tri_length && n->tri_linear && n->tri_timer >= 2) {
        int32_t per = (n->tri_timer + 1) * 3;
        n->tri_acc += dt;
        if (n->tri_acc >= per) {
            int32_t k = n->tri_acc / per;
            n->tri_acc -= k * per;
            n->tri_step = (n->tri_step + k) & 31;
        }
    }
    int t = apu_tri[n->tri_step];
    int32_t nper = apu_noise_period[n->noise_period_ix] * 3;
    n->noise_acc += dt;
    int shift = n->noise_mode ? 6 : 1;
    while (n->noise_acc >= nper) {
        n->noise_acc -= nper;
        uint16_t l = n->noise_lfsr;
        n->noise_lfsr = (l >> 1) | ((((l ^ (l >> shift)) & 1)) << 14);
    }
    int nz = 0;
    if (n->noise_length && !(n->noise_lfsr & 1)) {
        nz = n->noise_constvol ? n->noise_vol : n->noise_env_decay;
    }
    int32_t dper = apu_dmc_rate[n->dmc_rate_ix] * 3;
    n->dmc_acc += dt;
    while (n->dmc_acc >= dper) {
        n->dmc_acc -= dper;
        dmc_clock(n);
    }
    int32_t x = apu_pulse_table[p] + apu_tnd_table[3 * t + 2 * nz + n->dmc_level];
    // Track and remove DC so silence sits at zero and pops stay small.
    n->dc += (x * 256 - n->dc) >> 9;
    int32_t y = x - (n->dc >> 8);
    if (y > 32767) {
        y = 32767;
    } else if (y < -32768) {
        y = -32768;
    }
    if (n->sample_ix < NES_AUDIO_SAMPLES) {
        n->audio[n->sample_ix] = (int16_t)y;
    }
}

static int32_t sample_dot(uint32_t k) {
    return (int32_t)(((k + 1) * (uint32_t)NES_FRAME_DOTS) / NES_AUDIO_SAMPLES);
}

// Bring the APU up to dot `now` of the current frame.
static void apu_catchup(nes_t *n, int32_t now) {
    for (;;) {
        int32_t ts = n->sample_ix < NES_AUDIO_SAMPLES ? sample_dot(n->sample_ix) : 0x7FFFFFFF;
        int32_t tf = n->fc_next;
        if (tf <= ts && tf <= now) {
            apu_frame_step(n);
        } else if (ts <= now) {
            apu_sample(n, ts - n->apu_dot);
            n->apu_dot = ts;
            n->sample_ix++;
        } else {
            break;
        }
    }
}

static void apu_write(nes_t *n, uint16_t addr, uint8_t val) {
    apu_catchup(n, nes_dot(n));
    if (addr < 0x4008) {
        apu_pulse_t *p = &n->pulse[(addr >> 2) & 1];
        switch (addr & 3) {
            case 0:
                p->duty = val >> 6;
                p->halt = (val >> 5) & 1;
                p->constvol = (val >> 4) & 1;
                p->vol = val & 15;
                break;
            case 1:
                p->sw_en = val >> 7;
                p->sw_period = (val >> 4) & 7;
                p->sw_neg = (val >> 3) & 1;
                p->sw_shift = val & 7;
                p->sw_reload = 1;
                break;
            case 2:
                p->timer = (p->timer & 0x700) | val;
                break;
            default:
                p->timer = (p->timer & 0xFF) | ((val & 7) << 8);
                if (p->enabled) {
                    p->length = apu_length_table[val >> 3];
                }
                p->step = 0;
                p->env_start = 1;
                break;
        }
        return;
    }
    switch (addr) {
        case 0x4008:
            n->tri_halt = val >> 7;
            n->tri_linear_reload = val & 0x7F;
            break;
        case 0x400A:
            n->tri_timer = (n->tri_timer & 0x700) | val;
            break;
        case 0x400B:
            n->tri_timer = (n->tri_timer & 0xFF) | ((val & 7) << 8);
            if (n->tri_enabled) {
                n->tri_length = apu_length_table[val >> 3];
            }
            n->tri_reload_flag = 1;
            break;
        case 0x400C:
            n->noise_halt = (val >> 5) & 1;
            n->noise_constvol = (val >> 4) & 1;
            n->noise_vol = val & 15;
            break;
        case 0x400E:
            n->noise_mode = val >> 7;
            n->noise_period_ix = val & 15;
            break;
        case 0x400F:
            if (n->noise_enabled) {
                n->noise_length = apu_length_table[val >> 3];
            }
            n->noise_env_start = 1;
            break;
        case 0x4010:
            n->dmc_irq_en = val >> 7;
            n->dmc_loop = (val >> 6) & 1;
            n->dmc_rate_ix = val & 15;
            if (!n->dmc_irq_en) {
                n->irq &= ~4;
            }
            break;
        case 0x4011:
            n->dmc_level = val & 0x7F;
            break;
        case 0x4012:
            n->dmc_addr_start = 0xC000 | (val << 6);
            break;
        case 0x4013:
            n->dmc_len_start = (val << 4) | 1;
            break;
        case 0x4015:
            n->pulse[0].enabled = val & 1;
            n->pulse[1].enabled = (val >> 1) & 1;
            n->tri_enabled = (val >> 2) & 1;
            n->noise_enabled = (val >> 3) & 1;
            if (!n->pulse[0].enabled) {
                n->pulse[0].length = 0;
            }
            if (!n->pulse[1].enabled) {
                n->pulse[1].length = 0;
            }
            if (!n->tri_enabled) {
                n->tri_length = 0;
            }
            if (!n->noise_enabled) {
                n->noise_length = 0;
            }
            n->irq &= ~4;
            if (!(val & 0x10)) {
                n->dmc_left = 0;
            } else if (n->dmc_left == 0) {
                n->dmc_addr = n->dmc_addr_start;
                n->dmc_left = n->dmc_len_start;
            }
            break;
        case 0x4017:
            n->fc_mode = val >> 7;
            n->fc_inhibit = (val >> 6) & 1;
            if (n->fc_inhibit) {
                n->irq &= ~2;
            }
            n->fc_step = 0;
            n->fc_next = nes_dot(n) + (n->fc_mode ? apu_fc5[0] : apu_fc4[0]);
            if (n->fc_mode) {
                apu_quarter(n);
                apu_half(n);
            }
            break;
        default:
            break;
    }
}

static uint8_t apu_status(nes_t *n) {
    apu_catchup(n, nes_dot(n));
    uint8_t r = (n->pulse[0].length ? 1 : 0) | (n->pulse[1].length ? 2 : 0) |
        (n->tri_length ? 4 : 0) | (n->noise_length ? 8 : 0) | (n->dmc_left ? 0x10 : 0) |
        ((n->irq & 2) ? 0x40 : 0) | ((n->irq & 4) ? 0x80 : 0);
    n->irq &= ~2;
    return r;
}

// ---------------------------------------------------------------------------
// PPU

static inline uint8_t *pal_slot(nes_t *n, uint16_t addr) {
    uint16_t i = addr & 0x1F;
    if ((i & 0x13) == 0x10) {
        i &= 0x0F;
    }
    return &n->pal[i];
}

static uint8_t ppu_mem_read(nes_t *n, uint16_t addr) {
    addr &= 0x3FFF;
    if (addr < 0x2000) {
        return n->chr[addr >> 10][addr & 0x3FF];
    }
    if (addr < 0x3F00) {
        return n->nt[(addr >> 10) & 3][addr & 0x3FF];
    }
    return *pal_slot(n, addr);
}

static void ppu_mem_write(nes_t *n, uint16_t addr, uint8_t val) {
    addr &= 0x3FFF;
    if (addr < 0x2000) {
        if (n->chr_is_ram) {
            n->chr[addr >> 10][addr & 0x3FF] = val;
        }
    } else if (addr < 0x3F00) {
        n->nt[(addr >> 10) & 3][addr & 0x3FF] = val;
    } else {
        *pal_slot(n, addr) = val & 0x3F;
        n->colors_dirty = 1;
    }
}

static uint8_t ppu_read(nes_t *n, uint16_t reg) {
    switch (reg & 7) {
        case 2: {
            uint8_t r = (n->status & 0xE0) | (n->latch & 0x1F);
            n->status &= ~0x80;
            n->w = 0;
            n->latch = r;
            return r;
        }
        case 4:
            n->latch = n->oam[n->oamaddr];
            return n->latch;
        case 7: {
            uint16_t a = n->v & 0x3FFF;
            uint8_t r;
            if (a >= 0x3F00) {
                r = (n->latch & 0xC0) | *pal_slot(n, a);
                n->readbuf = ppu_mem_read(n, a - 0x1000);
            } else {
                r = n->readbuf;
                n->readbuf = ppu_mem_read(n, a);
            }
            n->v = (n->v + ((n->ctrl & 4) ? 32 : 1)) & 0x7FFF;
            n->latch = r;
            return r;
        }
        default:
            return n->latch;
    }
}

static void ppu_write(nes_t *n, uint16_t reg, uint8_t val) {
    n->latch = val;
    switch (reg & 7) {
        case 0:
            if (!(n->ctrl & 0x80) && (val & 0x80) && (n->status & 0x80)) {
                n->nmi = 1;
            }
            n->ctrl = val;
            n->t = (n->t & 0xF3FF) | ((val & 3) << 10);
            break;
        case 1:
            n->mask = val;
            n->colors_dirty = 1;
            break;
        case 3:
            n->oamaddr = val;
            break;
        case 4:
            n->oam[n->oamaddr++] = val;
            break;
        case 5:
            if (!n->w) {
                n->t = (n->t & 0xFFE0) | (val >> 3);
                n->fine_x = val & 7;
            } else {
                n->t = (n->t & 0x8C1F) | ((val & 7) << 12) | ((val & 0xF8) << 2);
            }
            n->w ^= 1;
            break;
        case 6:
            if (!n->w) {
                n->t = (n->t & 0x00FF) | ((val & 0x3F) << 8);
            } else {
                n->t = (n->t & 0xFF00) | val;
                n->v = n->t;
            }
            n->w ^= 1;
            break;
        case 7:
            ppu_mem_write(n, n->v, val);
            n->v = (n->v + ((n->ctrl & 4) ? 32 : 1)) & 0x7FFF;
            break;
        default:
            break;
    }
}

static void ppu_inc_vert(nes_t *n) {
    uint16_t v = n->v;
    if ((v & 0x7000) != 0x7000) {
        v += 0x1000;
    } else {
        v &= ~0x7000;
        int y = (v & 0x3E0) >> 5;
        if (y == 29) {
            y = 0;
            v ^= 0x800;
        } else if (y == 31) {
            y = 0;
        } else {
            y++;
        }
        v = (v & ~0x3E0) | (y << 5);
    }
    n->v = v;
}

// Draw one visible line from the current PPU state and hand it to
// nes_draw_line as 256 palette slots (0..31) plus the 32 NES colors those
// slots map to. Records the sprite 0 hit dot in n->s0_dot.
static void ppu_render_line(nes_t *n, int line) {
    uint8_t *colors = n->colors;
    n->s0_dot = -1;
    if (!(n->mask & 0x18)) {
        // Rendering off: backdrop, or the palette entry v points at.
        colors[0] = n->pal[0];
        if ((n->v & 0x3F00) == 0x3F00) {
            colors[0] = *pal_slot(n, n->v);
        }
        n->colors_dirty = 1;
        n->colors_ver++;
        memset(n->line, 0, NES_W);
        nes_draw_line(n, line, n->line, colors);
        return;
    }
    if (n->colors_dirty) {
        uint8_t grey = (n->mask & 1) ? 0x30 : 0x3F;
        for (int i = 0; i < 32; i++) {
            colors[i] = n->pal[(i & 3) ? i : 0] & grey;
        }
        n->colors_dirty = 0;
        n->colors_ver++;
    }
    // Background: 33 tiles into bg[8 - fine_x ...], slots 0..15 (0 = clear).
    // nes_spread turns a pattern byte into 8 pixel bytes of 0/1, so a tile row
    // is two table lookups, an OR, and a palette merge on the nonzero bytes.
    uint8_t *bg = n->bg;
    if (n->mask & 0x08) {
        uint16_t v = n->v;
        uint8_t *const *cb = &n->chr[(n->ctrl & 0x10) ? 4 : 0];
        uint32_t fy = (v >> 12) & 7;
        uint32_t cx = v & 31;
        uint32_t cy = (v >> 5) & 31;
        uint32_t ysh = (cy & 2) << 1;
        uint32_t ntsel = (v >> 10) & 3;
        const uint8_t *row = n->nt[ntsel] + cy * 32;
        const uint8_t *arow = n->nt[ntsel] + 0x3C0 + (cy >> 2) * 8;
        uint8_t *dst = bg + 8 - n->fine_x;
        for (int tile = 0; tile < 33; tile++) {
            uint32_t idx = row[cx];
            uint32_t pal = ((arow[cx >> 2] >> (ysh | (cx & 2))) & 3) << 2;
            const uint8_t *chr = cb[idx >> 6] + ((idx & 63) << 4) + fy;
            const uint32_t *lo = nes_spread[chr[0]];
            const uint32_t *hi = nes_spread[chr[8]];
            uint32_t p0 = lo[0] | (hi[0] << 1);
            uint32_t p1 = lo[1] | (hi[1] << 1);
            p0 |= ((p0 | (p0 >> 1)) & 0x01010101) * pal;
            p1 |= ((p1 | (p1 >> 1)) & 0x01010101) * pal;
            memcpy(dst, &p0, 4);
            memcpy(dst + 4, &p1, 4);
            dst += 8;
            if (++cx == 32) {
                // Wrap into the horizontally adjacent nametable.
                cx = 0;
                ntsel ^= 1;
                row = n->nt[ntsel] + cy * 32;
                arow = n->nt[ntsel] + 0x3C0 + (cy >> 2) * 8;
            }
        }
        if (!(n->mask & 0x02)) {
            memset(bg + 8, 0, 8);
        }
    } else {
        memset(bg + 8, 0, NES_W);
    }
    bg += 8;

    // Sprites: the first 8 in OAM order on this line, drawn straight onto the
    // background slots. A lower OAM index owns a pixel even when it is behind
    // the background; sp[] marks owned pixels with a per-line tag so it never
    // needs clearing.
    if (n->mask & 0x10) {
        int h = (n->ctrl & 0x20) ? 16 : 8;
        int count = 0;
        uint8_t *sp = n->sp;
        uint8_t tag = ++n->sp_tag;
        if (tag == 0) {
            memset(sp, 0, NES_W);
            tag = n->sp_tag = 1;
        }
        int xmin = (n->mask & 0x04) ? 0 : 8;
        for (int i = 0; i < 64; i++) {
            const uint8_t *o = &n->oam[i * 4];
            int row = line - 1 - o[0];
            if ((unsigned)row >= (unsigned)h) {
                continue;
            }
            if (++count > 8) {
                n->status |= 0x20;
                break;
            }
            uint8_t attr = o[2];
            if (attr & 0x80) {
                row = h - 1 - row;
            }
            uint16_t pa;
            if (h == 16) {
                pa = ((o[1] & 1) << 12) | ((o[1] & 0xFE) << 4);
                if (row >= 8) {
                    pa += 16;
                    row -= 8;
                }
            } else {
                pa = ((n->ctrl & 0x08) << 9) | (o[1] << 4);
            }
            pa |= row;
            const uint8_t *chr = n->chr[pa >> 10];
            uint32_t lo = chr[pa & 0x3FF];
            uint32_t hi = chr[(pa & 0x3FF) + 8];
            if (!(lo | hi)) {
                continue;
            }
            if (!(attr & 0x40)) {
                // Unflipped: leftmost pixel is bit 7; reverse so bit k is pixel k.
                lo = nes_rev8[lo];
                hi = nes_rev8[hi];
            }
            uint8_t pal = 0x10 | ((attr & 3) << 2);
            int behind = attr & 0x20;
            int x = o[3];
            for (int k = 0; k < 8; k++, x++, lo >>= 1, hi >>= 1) {
                uint8_t px = (lo & 1) | ((hi & 1) << 1);
                if (!px || x >= NES_W || x < xmin || sp[x] == tag) {
                    continue;
                }
                sp[x] = tag;
                uint8_t b = bg[x];
                if (i == 0 && b && x != 255 && n->s0_dot < 0) {
                    n->s0_dot = x;
                }
                if (!b || !behind) {
                    bg[x] = pal | px;
                }
            }
        }
    }
    nes_draw_line(n, line, bg, colors);
}

// ---------------------------------------------------------------------------
// CPU bus

static uint8_t io_read(nes_t *n, uint16_t addr) {
    if (addr < 0x4000) {
        return ppu_read(n, addr);
    }
    if (addr == 0x4016) {
        uint8_t r;
        if (n->strobe) {
            r = n->pad & 1;
        } else {
            r = n->pad_shift & 1;
            n->pad_shift = (n->pad_shift >> 1) | 0x80;
        }
        return 0x40 | r;
    }
    if (addr == 0x4017) {
        return 0x40;
    }
    if (addr == 0x4015) {
        return apu_status(n);
    }
    if (addr >= 0x6000) {
        return n->prg_ram[addr & 0x1FFF];
    }
    return 0x40;  // open bus approximation
}

static uint8_t cpu_peek(nes_t *n, uint16_t addr) {
    if (addr >= 0x8000) {
        return n->prg[(addr >> 13) & 3][addr & 0x1FFF];
    }
    if (addr < 0x2000) {
        return n->ram[addr & 0x7FF];
    }
    if (addr >= 0x6000) {
        return n->prg_ram[addr & 0x1FFF];
    }
    return 0;
}

static void io_write(nes_t *n, uint16_t addr, uint8_t val) {
    if (addr < 0x4000) {
        ppu_write(n, addr, val);
    } else if (addr == 0x4014) {
        // OAM DMA: 513 cycles, 514 on an odd cycle.
        uint16_t src = val << 8;
        for (int i = 0; i < 256; i++) {
            n->oam[(uint8_t)(n->oamaddr + i)] = src < 0x2000 ? n->ram[(src + i) & 0x7FF]
                : cpu_peek(n, src + i);
        }
        n->cyc += 513 + (n->cyc & 1);
    } else if (addr == 0x4016) {
        n->strobe = val & 1;
        if (n->strobe) {
            n->pad_shift = n->pad;
        }
    } else if (addr < 0x4018) {
        apu_write(n, addr, val);
    } else if (addr >= 0x8000) {
        mapper_write(n, addr, val);
    } else if (addr >= 0x6000) {
        n->prg_ram[addr & 0x1FFF] = val;
    }
}

// ---------------------------------------------------------------------------
// CPU

#define GETP() ((nr & 0x80) | (fv << 6) | 0x20 | (fd << 3) | (fi << 2) | ((zr == 0) << 1) | fc)
#define SETP(p) do { uint8_t p_ = (p); nr = p_; zr = !(p_ & 2); fv = (p_ >> 6) & 1; \
                     fd = (p_ >> 3) & 1; fi = (p_ >> 2) & 1; fc = p_ & 1; } while (0)
#define NZ(v) (nr = zr = (v))
#define ADC(mv) do { uint8_t m_ = (mv); unsigned t_ = a + m_ + fc; \
                     fv = ((~(a ^ m_) & (a ^ t_)) >> 7) & 1; fc = t_ >> 8; a = (uint8_t)t_; NZ(a); } while (0)
#define CMP(r, mv) do { uint8_t m_ = (mv); fc = (r) >= m_; NZ((uint8_t)((r) - m_)); } while (0)
#define PUSH(v) (ram[0x100 | s--] = (uint8_t)(v))
#define PULL() (ram[0x100 | ++s])
#define FETCH(ad) ((ad) >= 0x8000 ? n->prg[((ad) >> 13) & 3][(ad) & 0x1FFF] : RD(ad))
#define FETCH16(ad) (FETCH(ad) | (FETCH((uint16_t)((ad) + 1)) << 8))
#define RD16(ad) (RD(ad) | (RD((uint16_t)((ad) + 1)) << 8))
#define RD(ad) rd(n, (ad), &cyc, &poll)
#define WR(ad, v) wr(n, (ad), (v), &cyc, &poll)

// I/O can raise NMI or IRQ, so the slow paths set *poll to make cpu_run look
// at the interrupt lines before the next instruction.
static inline uint8_t rd(nes_t *n, uint16_t addr, int32_t *cyc, uint8_t *poll) {
    if (addr < 0x2000) {
        return n->ram[addr & 0x7FF];
    }
    if (addr >= 0x8000) {
        return n->prg[(addr >> 13) & 3][addr & 0x1FFF];
    }
    n->cyc = *cyc;
    uint8_t r = io_read(n, addr);
    *cyc = n->cyc;
    *poll = 1;
    return r;
}

static inline void wr(nes_t *n, uint16_t addr, uint8_t val, int32_t *cyc, uint8_t *poll) {
    if (addr < 0x2000) {
        n->ram[addr & 0x7FF] = val;
        return;
    }
    n->cyc = *cyc;
    io_write(n, addr, val);
    *cyc = n->cyc;
    *poll = 1;
}

// Run instructions until the cycle counter reaches `target`.
static void cpu_run(nes_t *n, int32_t target) {
    uint16_t pc = n->pc;
    uint8_t a = n->a, x = n->x, y = n->y, s = n->s;
    uint8_t fc = n->fc, fv = n->fv, fi = n->fi, fd = n->fd, nr = n->nr, zr = n->zr;
    uint8_t *ram = n->ram;
    int32_t cyc = n->cyc;
#ifdef HOST_TEST
    uint32_t instrs = 0;
#endif
    // Interrupt lines only change between cpu_run calls, during I/O, or when
    // the I flag clears (CLI, PLP, RTI), so they are checked only then.
    uint8_t poll = 1;
    while (cyc < target) {
        if (poll) {
            poll = 0;
            uint16_t vec = 0;
            if (n->nmi) {
                n->nmi = 0;
                vec = 0xFFFA;
            } else if (n->irq && !fi) {
                vec = 0xFFFE;
            }
            if (vec) {
                poll = 1;
                PUSH(pc >> 8);
                PUSH(pc & 0xFF);
                PUSH(GETP());
                fi = 1;
                pc = RD16(vec);
                cyc += 7;
                continue;
            }
        }
        uint16_t ea;
        uint8_t op = FETCH(pc);
        pc++;
#ifdef HOST_TEST
        instrs++;
#endif
        switch (op) {
            #include "nes_ops.inc"
        }
        continue;
    jam:
        n->err = 1;
        n->err_addr = pc - 1;
        break;
    }
    n->pc = pc;
    n->a = a;
    n->x = x;
    n->y = y;
    n->s = s;
    n->fc = fc;
    n->fv = fv;
    n->fi = fi;
    n->fd = fd;
    n->nr = nr;
    n->zr = zr;
    n->cyc = cyc;
#ifdef HOST_TEST
    n->instrs += instrs;
#endif
}

#ifdef HOST_TEST
// Status register as pushed by PHP without B, for the nestest trace.
static uint8_t nes_get_p(const nes_t *n) {
    uint8_t nr = n->nr, zr = n->zr, fv = n->fv, fd = n->fd, fi = n->fi, fc = n->fc;
    return GETP();
}
#endif

// ---------------------------------------------------------------------------
// Machine

static void run_to_dot(nes_t *n, int32_t dot) {
    if (n->err) {
        return;
    }
    cpu_run(n, (dot + n->phase + 2) / 3);
}

static void nes_reset(nes_t *n) {
    n->s = 0xFD;
    n->fi = 1;
    n->zr = 1;
    n->pc = cpu_peek(n, 0xFFFC) | (cpu_peek(n, 0xFFFD) << 8);
    n->noise_lfsr = 1;
    n->dmc_bits = 8;
    n->dmc_silence = 1;
    n->fc_next = apu_fc4[0];
    n->dc = apu_tnd_table[3 * 15] * 256;  // idle triangle level, no pop at start
    n->colors_dirty = 1;
    n->cyc = 7;
}

// Parse the iNES image and power on. Returns NES_OK or an NES_ERR_* code.
static int nes_init(nes_t *n, const uint8_t *rom, uint32_t len) {
    memset(n, 0, sizeof(*n));
    if (len < 16 || rom[0] != 'N' || rom[1] != 'E' || rom[2] != 'S' || rom[3] != 0x1A) {
        return NES_ERR_HEADER;
    }
    uint8_t f6 = rom[6], f7 = rom[7];
    // Old dumps have junk ("DiskDude!") in bytes 7..15; trust byte 7 only if
    // it is NES 2.0 or bytes 12..15 are clear.
    if ((f7 & 0x0C) != 0x08 && (rom[12] | rom[13] | rom[14] | rom[15])) {
        f7 = 0;
    }
    n->mapper = (f6 >> 4) | (f7 & 0xF0);
    n->battery = (f6 >> 1) & 1;
    n->four_screen = (f6 >> 3) & 1;
    n->prg_size = rom[4] * 0x4000u;
    n->chr_size = rom[5] * 0x2000u;
    uint32_t off = 16 + ((f6 & 4) ? 512 : 0);
    if (n->prg_size == 0 || off + n->prg_size + n->chr_size > len) {
        return NES_ERR_HEADER;
    }
    switch (n->mapper) {
        case 0: case 1: case 2: case 3: case 4: case 7: case 66:
            break;
        default:
            return NES_ERR_MAPPER;
    }
    n->prg_rom = rom + off;
    n->chr_rom = rom + off + n->prg_size;
    if (n->chr_size == 0) {
        n->chr_is_ram = 1;
        n->chr_size = 0x2000;
    }
    n->prg_ram = n->prg_ram_int;
    set_mirror(n, (f6 & 1) ? 2 : 3);
    mapper_reset(n);
    nes_reset(n);
    return NES_OK;
}

// Emulate one video frame: lines 0-239 visible, 240 idle, 241-260 vblank,
// 261 pre-render. Audio for the frame lands in n->audio.
static void nes_run_frame(nes_t *n) {
    for (int line = 0; line < 240 && !n->err; line++) {
        int32_t base = line * 341;
        int on = n->mask & 0x18;
        ppu_render_line(n, line);
        if (n->s0_dot >= 0 && (n->mask & 0x18) == 0x18) {
            run_to_dot(n, base + n->s0_dot + 2);
            n->status |= 0x40;
        }
        run_to_dot(n, base + 257);
        if (on) {
            ppu_inc_vert(n);
            n->v = (n->v & ~0x041F) | (n->t & 0x041F);
        }
        run_to_dot(n, base + 260);
        if (on) {
            mapper_scanline(n);
        }
        run_to_dot(n, base + 341);
        apu_catchup(n, nes_dot(n));
    }
    run_to_dot(n, 241 * 341 + 1);
    n->status |= 0x80;
    if (n->ctrl & 0x80) {
        n->nmi = 1;
    }
    for (int line = 241; line < 261; line++) {
        run_to_dot(n, (line + 1) * 341);
        apu_catchup(n, nes_dot(n));
    }
    int32_t base = 261 * 341;
    run_to_dot(n, base + 1);
    n->status &= 0x1F;
    int on = n->mask & 0x18;
    run_to_dot(n, base + 257);
    if (on) {
        n->v = (n->v & ~0x041F) | (n->t & 0x041F);
    }
    run_to_dot(n, base + 260);
    if (on) {
        mapper_scanline(n);
    }
    run_to_dot(n, base + 280);
    if (on) {
        n->v = (n->v & ~0x7BE0) | (n->t & 0x7BE0);
    }
    // Odd frames are one dot short while rendering.
    int32_t frame_dots = NES_FRAME_DOTS - ((n->frame & 1) && on ? 1 : 0);
    run_to_dot(n, frame_dots);
    apu_catchup(n, NES_FRAME_DOTS);
    // Carry the overshoot into the next frame.
    int32_t q = (frame_dots + n->phase) / 3;
    n->cyc -= q;
    n->phase = (frame_dots + n->phase) % 3;
    n->fc_next -= frame_dots;
    n->apu_dot -= frame_dots;
    n->sample_ix = 0;
    n->frame++;
}
