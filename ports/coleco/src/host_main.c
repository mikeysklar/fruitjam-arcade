// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Host test harness for coleco.c. Build: make -f Makefile.host
//
//   host_coleco run cart.col frames [fbdump.txt] [out.wav] [script]
//
// Prints the frame CRC every 60 frames and a BENCH line. fbdump.txt gets the
// 320x240 RGB565 framebuffer in the same BEGIN_FB/base64/END_FB form code.py
// prints over serial, for tools/fb2png.py. script is a comma separated list
// of first-last:hexmask input holds for controller 1, e.g. "120-125:10".
// COLECO_BIOS=path uses that 8 KB BIOS file instead of the built-in one.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define Z80_COUNT
#include "coleco.c"

static uint8_t rom[1 << 20];
static uint8_t bios[8192];
static cv_t cv;
static uint16_t fb[320 * 240];

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

static uint32_t script_mask(const char *s, uint32_t f) {
    uint32_t m = 0;
    while (s && *s) {
        unsigned a, b, mask;
        if (sscanf(s, "%u-%u:%x", &a, &b, &mask) == 3 && f >= a && f <= b) {
            m |= mask;
        }
        s = strchr(s, ',');
        if (s) {
            s++;
        }
    }
    return m;
}

// VDP sprite and interrupt checks on a hand-built VRAM, no cartridge needed.
static int check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    return !ok;
}

static void spr(cv_t *m, int i, int y, int x, int name, int col) {
    uint8_t *a = m->vram + 0x1B00 + i * 4;
    a[0] = y;
    a[1] = x;
    a[2] = name;
    a[3] = col;
}

static int selftest(void) {
    static uint8_t cart[8192];
    int bad = 0;
    cv_t *m = &cv;
    memset(cart, 0xFF, sizeof cart);
    ctx_init(m, cart, sizeof cart, NULL);
    // Graphics I, display on, IE, 8x8 sprites; SAT $1B00, sprite patterns $3800.
    uint8_t regs[8] = {0x00, 0xE0, 0x06, 0x80, 0x00, 0x36, 0x07, 0x04};
    memcpy(m->reg, regs, 8);
    memset(m->vram + 0x3800, 0xFF, 8);  // pattern 0 solid
    spr(m, 0, 9, 0, 0, 2);
    spr(m, 1, 9, 20, 0, 3);
    spr(m, 2, 9, 40, 0, 4);
    spr(m, 3, 9, 60, 0, 5);
    spr(m, 4, 9, 80, 0, 6);
    spr(m, 5, 0xD0, 0, 0, 0);
    vdp_compose(m, 10);
    bad += check((m->status & 0x5F) == 0x44, "5th sprite flag and number 4");
    bad += check(m->line[60] == 5 && m->line[80] == 0, "only the first 4 sprites drawn");
    bad += check(!(m->status & 0x20), "no collision without overlap");
    uint8_t s = vdp_status(m);
    bad += check(s == 0x44 && m->status == 0x04, "status read clears 5S");
    m->status = 0;
    vdp_compose(m, 20);
    bad += check(m->status == 0x05, "no 5th sprite: number of the D0 terminator");
    spr(m, 4, 0xD0, 0, 0, 0);
    spr(m, 1, 9, 4, 0, 0);  // transparent, overlapping sprite 0
    m->status = 0;
    vdp_compose(m, 10);
    bad += check(m->status & 0x20, "collision with a color 0 sprite");
    bad += check(m->line[4] == 2 && m->line[10] == 0, "color 0 sprite not drawn, priority");
    spr(m, 1, 9, 40, 0, 0x83);  // early clock: x 40 - 32 = 8
    m->status = 0;
    vdp_compose(m, 10);
    bad += check(m->line[8] == 3 && !(m->status & 0x20), "EC shifts left 32");
    m->reg[1] |= 0x01;  // magnified
    spr(m, 1, 0xD0, 0, 0, 0);
    vdp_compose(m, 25);
    bad += check(m->line[15] == 2 && m->line[16] == 0, "magnified 8x8 is 16 wide");
    m->reg[1] &= ~0x01;
    spr(m, 0, 0xFF, 100, 0, 7);  // y = -1: first row on line 0
    vdp_compose(m, 0);
    bad += check(m->line[100] == 7, "Y 255 is line 0");
    // NMI: frame flag with IE gives one edge; turning IE on later gives another.
    m->cpu.nmi = 0;
    m->int_line = 0;
    m->status = 0x80;
    vdp_update_int(m);
    bad += check(m->cpu.nmi == 1, "frame flag raises NMI");
    m->cpu.nmi = 0;
    vdp_update_int(m);
    bad += check(m->cpu.nmi == 0, "level held, no second NMI");
    vdp_ctrl(m, 0xC0);
    vdp_ctrl(m, 0x81);  // IE off
    vdp_ctrl(m, 0xE0);
    vdp_ctrl(m, 0x81);  // IE on with the flag still set
    bad += check(m->cpu.nmi == 1, "enabling IE with flag set raises NMI");
    vdp_status(m);
    bad += check(m->int_line == 0, "status read drops the interrupt line");
    // Keypad and joystick reads.
    m->input[0] = CV_KEY(11) | CV_FIRE_R;
    m->joy_mode = 0;
    bad += check(cv_pad(m, 0) == 0x36, "keypad # with right fire");
    m->input[0] = CV_UP | CV_LEFT | CV_FIRE_L;
    m->joy_mode = 1;
    bad += check(cv_pad(m, 0) == 0x36, "joystick up+left with left fire");
    printf("SELFTEST %s\n", bad ? "FAIL" : "PASS");
    return bad;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "selftest")) {
        return selftest();
    }
    if (argc < 4 || strcmp(argv[1], "run")) {
        fprintf(stderr, "usage: host_coleco run cart.col frames [fbdump] [wav] [script]\n");
        return 2;
    }
    FILE *fp = fopen(argv[2], "rb");
    if (!fp) {
        perror(argv[2]);
        return 2;
    }
    size_t len = fread(rom, 1, sizeof rom, fp);
    fclose(fp);
    size_t unit = len > 0x8000 ? 0x4000 : 0x2000;
    while (len % unit) {
        rom[len++] = 0xFF;
    }
    const uint8_t *bp = NULL;
    if (getenv("COLECO_BIOS")) {
        FILE *bf = fopen(getenv("COLECO_BIOS"), "rb");
        if (!bf || fread(bios, 1, 8192, bf) != 8192) {
            fprintf(stderr, "bad BIOS file\n");
            return 2;
        }
        fclose(bf);
        bp = bios;
    }
    if (ctx_init(&cv, rom, len, bp)) {
        printf("init failed\n");
        return 1;
    }
    printf("cart %zu bytes, banks %u, header %s\n", len, cv.banks, cv_header_ok(&cv) ? "ok" : "MISSING");
    uint32_t frames = atoi(argv[3]);
    const char *script = argc > 6 ? argv[6] : NULL;
    cv.fb = getenv("CV_NOFB") ? NULL : (uint8_t *)fb;
    cv.fb_width = 320;
    cv.fb_height = 240;
    cv.bpp = 16;
    cv.x0 = 32;
    cv.y0 = 24;
    cv.crc_on = getenv("CV_NOCRC") == NULL;
    static int16_t ring[CV_AUDIO_SAMPLES * 2 * 3600];
    cv.ring = ring;
    cv.ring_frames = CV_AUDIO_SAMPLES * 3600;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint32_t f = 0; f < frames; f++) {
        cv.input[0] = script_mask(script, f);
        ctx_run_frames(&cv, 1);
        if ((f + 1) % 60 == 0) {
            printf("frame %u crc %08x pc %04x\n", f + 1, cv.last_crc, cv.cpu.pc);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("BENCH frames=%u crc=%08x host=%.3fs (%.0f fps, %.1f us/frame) instrs/frame=%.0f\n",
        frames, cv.last_crc, dt, frames / dt, dt * 1e6 / frames, (double)cv.cpu.instrs / frames);
    uint32_t nsamp = (frames < 3600 ? frames : 3600) * CV_AUDIO_SAMPLES;
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
    printf("VDP regs %02x %02x %02x %02x %02x %02x %02x %02x status %02x\n", cv.reg[0], cv.reg[1],
        cv.reg[2], cv.reg[3], cv.reg[4], cv.reg[5], cv.reg[6], cv.reg[7], cv.status);
    if (argc > 4 && argv[4][0]) {
        dump_fb(argv[4]);
    }
    if (argc > 5 && argv[5][0]) {
        FILE *w = fopen(argv[5], "wb");
        uint32_t bytes = nsamp * 4, rate = CV_AUDIO_RATE, br = rate * 4, riff = 36 + bytes, fmt = 16;
        uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
        fwrite("RIFF", 1, 4, w); fwrite(&riff, 4, 1, w); fwrite("WAVEfmt ", 1, 8, w); fwrite(&fmt, 4, 1, w);
        fwrite(&pcm, 2, 1, w); fwrite(&ch, 2, 1, w); fwrite(&rate, 4, 1, w); fwrite(&br, 4, 1, w);
        fwrite(&ba, 2, 1, w); fwrite(&bits, 2, 1, w);
        fwrite("data", 1, 4, w); fwrite(&bytes, 4, 1, w); fwrite(ring, 4, nsamp, w);
        fclose(w);
    }
    return 0;
}
