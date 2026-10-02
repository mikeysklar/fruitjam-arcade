// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Host test harness for msx.c. Build: make -f Makefile.host
//
//   host_msx run cart.rom frames [fbdump.txt] [out.wav] [script]
//   host_msx selftest
//
// Prints the frame CRC every 60 frames and a BENCH line. fbdump.txt gets the
// 320x240 RGB565 framebuffer in the same BEGIN_FB/base64/END_FB form code.py
// prints over serial, for ../tools/fb2png.py. script is a comma separated
// list of first-last:what holds, where what is a hex joystick 1 mask
// (e.g. "120-125:10" = trigger A) or k<row><bit> for a key (k80 = Space).
// MSX_BIOS=path uses that 32 KB BIOS instead of C-BIOS, MSX_MAPPER=n forces
// a mapper.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define Z80_COUNT
#include "msx.c"

static uint8_t rom[8 << 20];
static uint8_t bios[32768];
static msx_t mx;
static uint16_t fb[320 * 240];

static const char *mapper_name[MSX_MAP_COUNT] = {"plain", "konami", "konami-scc", "ascii8", "ascii16"};

static void dump_fb(const char *path) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    FILE *o = fopen(path, "w");
    const uint8_t *p = (const uint8_t *)fb;
    size_t n = sizeof fb, col = 0;
    fprintf(o, "some serial noise before\nBEGIN_FB 320 240 16\n");
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        char q[4] = {b64[(v >> 18) & 63], b64[(v >> 12) & 63], i + 1 < n ? b64[(v >> 6) & 63] : '=',
                     i + 2 < n ? b64[v & 63] : '='};
        for (int k = 0; k < 4; k++) {
            fputc(q[k], o);
            if (++col == 76) {
                fputc('\n', o);
                col = 0;
            }
        }
    }
    if (col) {
        fputc('\n', o);
    }
    fprintf(o, "END_FB\nfps 60.0\n");
    fclose(o);
}

// Apply the script's holds for frame f: joystick mask and keyboard rows.
static void script_input(const char *s, uint32_t f, msx_t *m) {
    m->joy[0] = 0;
    memset(m->kbd, 0, sizeof m->kbd);
    while (s && *s) {
        unsigned a, b, mask, row, bit;
        if (sscanf(s, "%u-%u:k%1x%1u", &a, &b, &row, &bit) == 4) {
            if (f >= a && f <= b && row < MSX_KBD_ROWS) {
                m->kbd[row] |= 1 << bit;
            }
        } else if (sscanf(s, "%u-%u:%x", &a, &b, &mask) == 3 && f >= a && f <= b) {
            m->joy[0] |= mask;
        }
        s = strchr(s, ',');
        if (s) {
            s++;
        }
    }
}

static int check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    return !ok;
}

static void spr(msx_t *m, int i, int y, int x, int name, int col) {
    uint8_t *a = m->vram + 0x1B00 + i * 4;
    a[0] = y;
    a[1] = x;
    a[2] = name;
    a[3] = col;
}

// Banked test ROM: every 8 KB bank starts with its number.
static uint8_t mrom[256 * 1024];

static int bank_at(msx_t *m, uint16_t a) {
    return msx_rd(m, a);
}

static int selftest(void) {
    int bad = 0;
    msx_t *m = &mx;
    // VDP: same checks as the coleco core, with the interrupt on INT.
    memset(mrom, 0xFF, 0x4000);
    ctx_init(m, mrom, 0x4000, NULL, -1);
    uint8_t regs[8] = {0x00, 0xE0, 0x06, 0x80, 0x00, 0x36, 0x07, 0x04};
    memcpy(m->reg, regs, 8);
    memset(m->vram + 0x3800, 0xFF, 8);
    spr(m, 0, 9, 0, 0, 2);
    spr(m, 1, 9, 20, 0, 3);
    spr(m, 2, 9, 40, 0, 4);
    spr(m, 3, 9, 60, 0, 5);
    spr(m, 4, 9, 80, 0, 6);
    spr(m, 5, 0xD0, 0, 0, 0);
    vdp_compose(m, 10);
    bad += check((m->status & 0x5F) == 0x44, "5th sprite flag and number 4");
    bad += check(m->line[60] == 5 && m->line[80] == 0, "only the first 4 sprites drawn");
    uint8_t s = vdp_status(m);
    bad += check(s == 0x44 && m->status == 0x04, "status read clears 5S");
    spr(m, 4, 0xD0, 0, 0, 0);
    spr(m, 1, 9, 4, 0, 0);
    m->status = 0;
    vdp_compose(m, 10);
    bad += check(m->status & 0x20, "collision with a color 0 sprite");
    m->status = 0x80;
    vdp_update_int(m);
    bad += check(m->cpu.irq == 1, "frame flag with IE drives INT");
    vdp_status(m);
    bad += check(m->cpu.irq == 0, "status read drops INT");
    m->status = 0x80;
    vdp_ctrl(m, 0x80);
    vdp_ctrl(m, 0x81);  // IE off
    bad += check(m->cpu.irq == 0, "IE off holds INT low");
    vdp_ctrl(m, 0xE0);
    vdp_ctrl(m, 0x81);
    bad += check(m->cpu.irq == 1, "IE on with flag set raises INT");

    // Slots: reset state is slot 0 everywhere (BIOS), RAM in slot 3.
    bad += check(msx_rd(m, 0) == cbios_main[0] && msx_rd(m, 0x7FFF) == cbios_main[0x7FFF], "BIOS in slot 0");
    bad += check(msx_rd(m, 0x8010) == cbios_logo[0x10], "logo ROM in slot 0 page 2");
    bad += check(msx_rd(m, 0xC000) == 0xFF, "slot 0 page 3 empty");
    msx_out(m, 0xA8, 0xFF);
    msx_wr(m, 0x1234, 0x5A);
    msx_wr(m, 0xF000, 0xA5);
    bad += check(msx_rd(m, 0x1234) == 0x5A && msx_rd(m, 0xF000) == 0xA5, "RAM in slot 3");
    msx_out(m, 0xA8, 0xF4);  // page 1 slot 1 (16 KB cart at $4000), page 0 BIOS
    bad += check(msx_rd(m, 0x4000) == 0xFF && msx_rd(m, 0) == cbios_main[0], "cart page 1, BIOS page 0");
    bad += check(msx_in(m, 0xA8) == 0xF4, "slot register reads back");
    mrom[0] = 'A';
    mrom[1] = 'B';
    bad += check(msx_rd(m, 0x4001) == 'B', "plain 16 KB at $4000");

    // PPI keyboard and click, PSG joystick port.
    msx_out(m, 0xAB, 0x82);
    m->kbd[8] = 0x01;  // Space
    msx_out(m, 0xAA, 0x08);
    bad += check(msx_in(m, 0xA9) == 0xFE, "keyboard row 8 Space");
    msx_out(m, 0xAB, 0x0F);  // set bit 7 (click)
    bad += check(m->ppi_c == 0x88, "PPI bit set");
    msx_out(m, 0xAB, 0x0E);
    bad += check(m->ppi_c == 0x08, "PPI bit reset");
    m->joy[0] = MSX_UP | MSX_TRIG_A;
    m->joy[1] = MSX_RIGHT;
    msx_out(m, 0xA0, 15);
    msx_out(m, 0xA1, 0x00);
    msx_out(m, 0xA0, 14);
    bad += check(msx_in(m, 0xA2) == 0x6E, "joystick 1 up + A on R14");
    msx_out(m, 0xA0, 15);
    msx_out(m, 0xA1, 0x40);
    msx_out(m, 0xA0, 14);
    bad += check(msx_in(m, 0xA2) == 0x77, "joystick 2 right on R14");
    msx_out(m, 0xA0, 1);
    msx_out(m, 0xA1, 0xFF);
    bad += check(msx_in(m, 0xA2) == 0x0F, "PSG register masks");

    // Mappers on a 256 KB ROM with numbered banks.
    for (int b = 0; b < 32; b++) {
        memset(mrom + b * 0x2000, b, 0x2000);
    }
    ctx_init(m, mrom, sizeof mrom, NULL, MSX_MAP_KONAMI);
    msx_out(m, 0xA8, 0x14 | 0xC0);  // pages 1, 2 slot 1, page 3 RAM
    bad += check(bank_at(m, 0x4000) == 0 && bank_at(m, 0x6000) == 1 && bank_at(m, 0xA000) == 3,
        "Konami initial banks 0-3");
    msx_wr(m, 0x4000, 9);
    msx_wr(m, 0x6000, 5);
    msx_wr(m, 0x8000, 6);
    msx_wr(m, 0xA000, 33);
    bad += check(bank_at(m, 0x4000) == 0 && bank_at(m, 0x6000) == 5 && bank_at(m, 0x8000) == 6 &&
        bank_at(m, 0xA000) == 1, "Konami $6000/$8000/$A000, $4000 fixed, wraps");
    bad += check(msx_rd(m, 0xC000) == m->ram[0xC000], "page 3 not cart");

    ctx_init(m, mrom, sizeof mrom, NULL, MSX_MAP_KONAMI_SCC);
    msx_out(m, 0xA8, 0x14 | 0xC0);
    msx_wr(m, 0x5000, 7);
    msx_wr(m, 0x7000, 8);
    msx_wr(m, 0x9000, 10);
    msx_wr(m, 0xB000, 11);
    msx_wr(m, 0x6000, 1);  // not a register on SCC carts
    bad += check(bank_at(m, 0x4000) == 7 && bank_at(m, 0x6000) == 8 && bank_at(m, 0x8000) == 10 &&
        bank_at(m, 0xA000) == 11, "Konami SCC $5000/$7000/$9000/$B000");
    msx_wr(m, 0x9000, 0x3F);
    msx_wr(m, 0x9800, 0x42);
    msx_wr(m, 0x9880, 0x10);
    msx_wr(m, 0x9881, 0x01);
    msx_wr(m, 0x988F, 0x01);
    bad += check(m->scc_on && msx_rd(m, 0x9800) == 0x42 && msx_rd(m, 0x9900) == 0x42 &&
        m->scc_per[0] == 0x110 && m->scc_en == 1, "SCC enable, wave and period");
    bad += check(msx_rd(m, 0x8000) == 0x3F % 32, "SCC on: $8000-$97FF still ROM");
    msx_wr(m, 0x9000, 2);
    bad += check(!m->scc_on && msx_rd(m, 0x9800) == 2, "SCC off");

    ctx_init(m, mrom, sizeof mrom, NULL, MSX_MAP_ASCII8);
    msx_out(m, 0xA8, 0x14 | 0xC0);
    msx_wr(m, 0x6000, 3);
    msx_wr(m, 0x6800, 4);
    msx_wr(m, 0x7000, 5);
    msx_wr(m, 0x7FFF, 6);
    bad += check(bank_at(m, 0x4000) == 3 && bank_at(m, 0x6000) == 4 && bank_at(m, 0x8000) == 5 &&
        bank_at(m, 0xA000) == 6, "ASCII8 $6000/$6800/$7000/$7800");

    ctx_init(m, mrom, sizeof mrom, NULL, MSX_MAP_ASCII16);
    msx_out(m, 0xA8, 0x14 | 0xC0);
    bad += check(bank_at(m, 0x4000) == 0 && bank_at(m, 0x8000) == 0, "ASCII16 initial bank 0");
    msx_wr(m, 0x6000, 2);
    msx_wr(m, 0x77FF, 5);
    msx_wr(m, 0x6800, 7);  // not a register
    bad += check(bank_at(m, 0x4000) == 4 && bank_at(m, 0x6000) == 5 && bank_at(m, 0x8000) == 10 &&
        bank_at(m, 0xA000) == 11, "ASCII16 $6000/$7000");

    // Mapper guesses from LD (nnnn),A counts.
    static const struct {
        uint16_t addr[3];
        uint32_t want;
    } g[] = {
        {{0x5000, 0x7000, 0x9000}, MSX_MAP_KONAMI_SCC},
        {{0x6000, 0x8000, 0xA000}, MSX_MAP_KONAMI},
        {{0x6000, 0x6800, 0x7800}, MSX_MAP_ASCII8},
        {{0x6000, 0x7000, 0x77FF}, MSX_MAP_ASCII16},
        {{0x6000, 0x7000, 0x1234}, MSX_MAP_ASCII16},
    };
    for (unsigned t = 0; t < sizeof g / sizeof g[0]; t++) {
        memset(mrom, 0, 0x20000);
        for (int k = 0; k < 3; k++) {
            mrom[k * 8] = 0x32;
            mrom[k * 8 + 1] = g[t].addr[k] & 0xFF;
            mrom[k * 8 + 2] = g[t].addr[k] >> 8;
        }
        char what[64];
        snprintf(what, sizeof what, "guess %s", mapper_name[g[t].want]);
        bad += check(msx_guess_mapper(mrom, 0x20000) == g[t].want, what);
    }
    // Plain ROM placement.
    memset(mrom, 0xFF, 0x10000);
    mrom[0x4000] = 'A';
    mrom[0x4001] = 'B';
    bad += check(msx_plain_base(mrom, 0xC000) == 0, "48 KB with header at $4000 starts at $0000");
    mrom[0] = 'A';
    mrom[1] = 'B';
    mrom[2] = 0x10;
    mrom[3] = 0x40;
    bad += check(msx_plain_base(mrom, 0x8000) == 0x4000, "32 KB at $4000");
    mrom[3] = 0x80;
    bad += check(msx_plain_base(mrom, 0x4000) == 0x8000, "16 KB starting in page 2 at $8000");
    // Sound: count rising edges over 60 frames (59.92 Hz) of rendered audio.
    ctx_init(m, mrom, 0x4000, NULL, MSX_MAP_PLAIN);
    static const uint8_t ay[][2] = {{0, 254}, {1, 0}, {7, 0x3E}, {8, 15}};
    for (unsigned k = 0; k < sizeof ay / sizeof ay[0]; k++) {
        msx_out(m, 0xA0, ay[k][0]);
        msx_out(m, 0xA1, ay[k][1]);
    }
    int edges = 0, prev = 0;
    for (int f = 0; f < 60; f++) {
        audio_render(m, MSX_AUDIO_SAMPLES);
        for (int i = 0; i < MSX_AUDIO_SAMPLES; i++) {
            int up = m->audio[i] > 0;
            edges += up && !prev;
            prev = up;
        }
        m->audio_pos = 0;
    }
    // 1789773 / (16 x 254) = 440.4 Hz, over 60 / 59.92 s.
    printf("     AY tone A period 254: %d edges\n", edges);
    bad += check(edges >= 438 && edges <= 444, "AY tone A at 440 Hz");
    msx_out(m, 0xA0, 8);
    msx_out(m, 0xA1, 0);
    for (int i = 0; i < 32; i++) {
        m->scc_wave[0][i] = i < 16 ? 100 : -100;
    }
    scc_write(m, 0x80, 0xFD);  // period 253: 3579545 / (32 x 254) = 440.4 Hz
    scc_write(m, 0x81, 0x00);
    scc_write(m, 0x8A, 15);
    scc_write(m, 0x8F, 1);
    edges = prev = 0;
    for (int f = 0; f < 60; f++) {
        m->audio_pos = 0;
        audio_render(m, MSX_AUDIO_SAMPLES);
        for (int i = 0; i < MSX_AUDIO_SAMPLES; i++) {
            int up = m->audio[i] > 0;
            edges += up && !prev;
            prev = up;
        }
    }
    printf("     SCC channel 1 period 253: %d edges\n", edges);
    bad += check(edges >= 438 && edges <= 444, "SCC channel 1 at 440 Hz");
    // Envelope: sawtooth up (shape 12) with period 256 on a steady channel A,
    // 1789773 / (256 x 256) = 27.3 ramps a second, each ending in a drop.
    m->scc_en = 0;
    static const uint8_t env[][2] = {{7, 0x3F}, {8, 0x10}, {11, 0}, {12, 1}, {13, 12}};
    for (unsigned k = 0; k < sizeof env / sizeof env[0]; k++) {
        msx_out(m, 0xA0, env[k][0]);
        msx_out(m, 0xA1, env[k][1]);
    }
    int drops = 0, last = 0;
    for (int f = 0; f < 60; f++) {
        m->audio_pos = 0;
        audio_render(m, MSX_AUDIO_SAMPLES);
        for (int i = 0; i < MSX_AUDIO_SAMPLES; i++) {
            drops += m->audio[i] - last < -2000;
            last = m->audio[i];
        }
    }
    printf("     envelope shape 12 period 256: %d drops\n", drops);
    bad += check(drops >= 26 && drops <= 29, "envelope sawtooth at 27.3 Hz");
    printf("SELFTEST %s\n", bad ? "FAIL" : "PASS");
    return bad;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "selftest")) {
        return selftest();
    }
    if (argc < 4 || strcmp(argv[1], "run")) {
        fprintf(stderr, "usage: host_msx run cart.rom frames [fbdump] [wav] [script]\n");
        return 2;
    }
    FILE *fp = fopen(argv[2], "rb");
    if (!fp) {
        perror(argv[2]);
        return 2;
    }
    size_t len = fread(rom, 1, sizeof rom, fp);
    fclose(fp);
    while (len % 0x2000) {
        rom[len++] = 0xFF;
    }
    const uint8_t *bp = NULL;
    if (getenv("MSX_BIOS")) {
        FILE *bf = fopen(getenv("MSX_BIOS"), "rb");
        if (!bf || fread(bios, 1, sizeof bios, bf) != sizeof bios) {
            fprintf(stderr, "bad BIOS file\n");
            return 2;
        }
        fclose(bf);
        bp = bios;
    }
    int mapper = getenv("MSX_MAPPER") ? atoi(getenv("MSX_MAPPER")) : -1;
    int err = ctx_init(&mx, rom, len, bp, mapper);
    if (err) {
        printf("init failed %d\n", err);
        return 1;
    }
    printf("cart %zu bytes, mapper %s, base %04x\n", len, mapper_name[mx.mapper], mx.rom_base);
    uint32_t frames = atoi(argv[3]);
    const char *script = argc > 6 ? argv[6] : NULL;
    mx.fb = getenv("MSX_NOFB") ? NULL : (uint8_t *)fb;
    mx.fb_width = 320;
    mx.fb_height = 240;
    mx.bpp = 16;
    mx.x0 = 32;
    mx.y0 = 24;
    mx.crc_on = getenv("MSX_NOCRC") == NULL;
    static int16_t ring[MSX_AUDIO_SAMPLES * 2 * 3600];
    mx.ring = ring;
    mx.ring_frames = MSX_AUDIO_SAMPLES * 3600;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint32_t f = 0; f < frames; f++) {
        script_input(script, f, &mx);
        ctx_run_frames(&mx, 1);
        if ((f + 1) % 60 == 0) {
            printf("frame %u crc %08x pc %04x slots %02x\n", f + 1, mx.last_crc, mx.cpu.pc, mx.slot_reg);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("BENCH frames=%u crc=%08x host=%.3fs (%.0f fps, %.1f us/frame) instrs/frame=%.0f\n",
        frames, mx.last_crc, dt, frames / dt, dt * 1e6 / frames, (double)mx.cpu.instrs / frames);
    uint32_t nsamp = (frames < 3600 ? frames : 3600) * MSX_AUDIO_SAMPLES;
    int peak = 0;
    uint32_t nz = 0;
    for (uint32_t i = 0; i < nsamp * 2; i++) {
        int v = ring[i] < 0 ? -ring[i] : ring[i];
        if (v > peak) {
            peak = v;
        }
        nz += v > 64;
    }
    printf("AUDIO samples=%u loud=%u peak=%d\n", nsamp, nz / 2, peak);
    printf("VDP regs %02x %02x %02x %02x %02x %02x %02x %02x status %02x\n", mx.reg[0], mx.reg[1],
        mx.reg[2], mx.reg[3], mx.reg[4], mx.reg[5], mx.reg[6], mx.reg[7], mx.status);
    if (argc > 4 && argv[4][0]) {
        dump_fb(argv[4]);
    }
    if (argc > 5 && argv[5][0]) {
        FILE *w = fopen(argv[5], "wb");
        uint32_t bytes = nsamp * 4, rate = MSX_AUDIO_RATE, br = rate * 4, riff = 36 + bytes, fmt = 16;
        uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
        fwrite("RIFF", 1, 4, w); fwrite(&riff, 4, 1, w); fwrite("WAVEfmt ", 1, 8, w); fwrite(&fmt, 4, 1, w);
        fwrite(&pcm, 2, 1, w); fwrite(&ch, 2, 1, w); fwrite(&rate, 4, 1, w); fwrite(&br, 4, 1, w);
        fwrite(&ba, 2, 1, w); fwrite(&bits, 2, 1, w);
        fwrite("data", 1, 4, w); fwrite(&bytes, 4, 1, w); fwrite(ring, 4, nsamp, w);
        fclose(w);
    }
    return 0;
}
