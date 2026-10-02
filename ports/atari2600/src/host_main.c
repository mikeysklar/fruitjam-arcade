// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Host reference for atari2600.c: runs a cartridge for N frames through the
// same RGB565 framebuffer path the board uses, prints frame CRCs and audio
// stats, optionally writes a PPM screenshot and a WAV.
//
//   host_main rom.bin [frames] [out.ppm] [out.wav]
//   host_main --check testrom.bin      pixel checks for testrom/make_testrom.py
//   host_main --banks dir              carts from testrom/make_banktests.py
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "atari2600.c"

#define FB_W 320
#define FB_H 240

static uint16_t fb[FB_W * FB_H];
static ctx_t ctx;
static int16_t ring[AUDIO_RATE * 120];

static int load(const char *path, uint8_t *rom, size_t cap, size_t *n) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return 1; }
    *n = fread(rom, 1, cap, fp);
    fclose(fp);
    return 0;
}

static void write_ppm(const char *path) {
    FILE *o = fopen(path, "wb");
    fprintf(o, "P6 %d %d 255\n", FB_W, FB_H);
    for (int i = 0; i < FB_W * FB_H; i++) {
        uint16_t c = fb[i];
        fputc((c >> 11) << 3, o); fputc(((c >> 5) & 63) << 2, o); fputc((c & 31) << 3, o);
    }
    fclose(o);
}

// Palette index of 2600 pixel x on framebuffer row y (fb is 2x wide).
static int px(int y, int x) {
    uint16_t c = fb[y * FB_W + 2 * x];
    for (int i = 0; i < 128; i++) {
        if ((uint16_t)ctx.pal32[i] == c) return i * 2;   // back to a COLUxx value
    }
    return -1;
}

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static int check_testrom(const char *path) {
    static uint8_t rom[4096];
    size_t n;
    if (load(path, rom, sizeof rom, &n)) return 2;
    if (a26_init(&ctx, rom, n)) { printf("init failed\n"); return 1; }
    a26_set_output(&ctx, (uint8_t *)fb, FB_W, FB_H, 16, 12);
    ctx.crc_on = 1;
    ctx.ring = ring; ctx.ring_len = sizeof ring / 2; ctx.ring_pos = 0;
    { char b[5]; mapper_name(&ctx, b); printf("mapper %s\n", b); }

    // Frames 1-4 no input, then fire held for frames 5-8.
    uint32_t crc3 = 0;
    for (int f = 1; f <= 4; f++) {
        a26_set_input(&ctx, 0);
        a26_run_frames(&ctx, 1);
    }
    crc3 = ctx.frame_crc;
    // Row layout: VSYNC ends on line 0, VBLANK off on line 37, section A is
    // lines 38-137, B is 138-157 (GRP written in hblank of each), C is 159-178 (odd lines 159, 161, ... change color, the next line resets it).
    int yA = 60 - 12, yB = 145 - 12, yC = 159 - 12;
    // Playfield blocks, reflected.
    int pf_on[] = {0, 3, 44, 47, 76, 79, 80, 83, 112, 115, 156, 159};
    int pf_off[] = {4, 43, 48, 75, 84, 111, 116, 155};
    for (unsigned i = 0; i < sizeof pf_on / sizeof pf_on[0]; i++) {
        CHECK(px(yA, pf_on[i]) == 0x1E, "PF on at x=%d got %02x", pf_on[i], px(yA, pf_on[i]));
    }
    for (unsigned i = 0; i < sizeof pf_off / sizeof pf_off[0]; i++) {
        CHECK(px(yA, pf_off[i]) == 0x84, "PF off (blue BK) at x=%d got %02x", pf_off[i], px(yA, pf_off[i]));
    }
    // P0 at 39, $C3 = pixels 39,40,45,46 (45,46 over PF: players win).
    int p0_on[] = {39, 40, 45, 46}, p0_off[] = {38, 41, 44};
    for (unsigned i = 0; i < 4; i++) CHECK(px(yB, p0_on[i]) == 0x44, "P0 at x=%d got %02x", p0_on[i], px(yB, p0_on[i]));
    CHECK(px(yB, p0_off[0]) == 0x84 && px(yB, p0_off[1]) == 0x84 && px(yB, p0_off[2]) == 0x1E,
          "P0 edges %02x %02x %02x", px(yB, 38), px(yB, 41), px(yB, 44));
    // P1 at 50 after HMOVE: 50,51,56,57. M0 at 56 is drawn over P1 there,
    // since P0/M0 have priority over P1/M1.
    int p1_on[] = {50, 51, 57};
    for (unsigned i = 0; i < 3; i++) CHECK(px(yB, p1_on[i]) == 0xC8, "P1 at x=%d got %02x", p1_on[i], px(yB, p1_on[i]));
    CHECK(px(yB, 56) == 0x44, "M0 over P1 at x=56 got %02x", px(yB, 56));
    CHECK(px(yB, 49) == 0x84 && px(yB, 52) == 0x84, "P1 edges %02x %02x", px(yB, 49), px(yB, 52));
    // Ball 65-68 in COLUPF.
    for (int x = 65; x <= 68; x++) CHECK(px(yB, x) == 0x1E, "BL at x=%d got %02x", x, px(yB, x));
    CHECK(px(yB, 64) == 0x84 && px(yB, 69) == 0x84, "BL edges %02x %02x", px(yB, 64), px(yB, 69));
    // Section C: new background from pixel 52.
    CHECK(px(yC, 51) == 0x84 && px(yC, 52) == 0x36, "mid-line COLUBK at 51/52: %02x %02x", px(yC, 51), px(yC, 52));
    // Objects off in section A.
    CHECK(px(yA, 39) == 0x84 && px(yA, 50) == 0x84, "no objects in section A");

    // Fire switches COLUP0.
    for (int f = 5; f <= 8; f++) {
        a26_set_input(&ctx, IN_FIRE);
        a26_run_frames(&ctx, 1);
    }
    CHECK(px(yB, 39) == 0x1E, "fire: P0 color %02x", px(yB, 39));
    uint32_t crc8 = ctx.frame_crc;
    // Audio: AUDC0=4 (pure tone, div 2), AUDF0=15, AUDV0=8: square wave at
    // 31399.5 / (2 * 16) = 981 Hz, level 8 * 1000.
    {
        uint32_t ns = ctx.nsamples, rises = 0;
        int peak = 0;
        for (uint32_t i = 1; i < ns; i++) {
            if (ring[i] > peak) peak = ring[i];
            rises += ring[i] > 4000 && ring[i - 1] <= 4000;
        }
        double hz = rises * (double)AUDIO_RATE / ns;
        printf("audio: %u samples, %.1f Hz square wave, peak %d\n", ns, hz, peak);
        CHECK(hz > 981 * 0.98 && hz < 981 * 1.02, "tone %.1f Hz, want 981", hz);
        CHECK(peak == 8000, "tone peak %d, want 8000", peak);
    }
    printf("frame 4 crc %08x, frame 8 crc %08x, frame lines %u, samples %u\n", crc3, crc8,
           ctx.line, ctx.nsamples);
    // Regression lock: CRCs of the RGB565 rows once the pixel checks above
    // passed. Update both if the palette changes on purpose.
    CHECK(crc3 == 0x46067099 && crc8 == 0x07337fd6, "frame CRCs changed");
    write_ppm("testrom.ppm");
    printf("testrom pixel checks: %s (%d failed), screenshot testrom.ppm\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}

// Bankswitch carts from testrom/make_banktests.py: detected scheme and the
// background color the landing bank sets, sampled on a middle row.
static int check_banks(const char *dir) {
    char list[512], name[64], mapper[8];
    unsigned color;
    snprintf(list, sizeof list, "%s/banks.txt", dir);
    FILE *f = fopen(list, "r");
    if (!f) { perror(list); return 2; }
    static uint8_t rom[64 << 10];
    while (fscanf(f, "%63s %7s %u", name, mapper, &color) == 3) {
        char path[600];
        size_t n;
        snprintf(path, sizeof path, "%s/%s", dir, name);
        if (load(path, rom, sizeof rom, &n)) return 2;
        memset(fb, 0, sizeof fb);
        int e = a26_init(&ctx, rom, n);
        a26_set_output(&ctx, (uint8_t *)fb, FB_W, FB_H, 16, 12);
        char got[5] = "";
        if (!e) mapper_name(&ctx, got);
        if (!e) a26_run_frames(&ctx, 3);
        int c = px(120, 80);
        int ok = !e && strcmp(got, mapper) == 0 && c == (int)color && !ctx.jammed;
        printf("  %-14s mapper %-5s (want %-5s) COLUBK %02x (want %02x) %s\n", name, got, mapper, c, color, ok ? "ok" : "FAIL");
        fails += !ok;
    }
    fclose(f);
    printf("bankswitch checks: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}

int main(int argc, char **argv) {
    if (argc > 2 && strcmp(argv[1], "--check") == 0) return check_testrom(argv[2]);
    if (argc > 2 && strcmp(argv[1], "--banks") == 0) return check_banks(argv[2]);
    if (argc < 2) { fprintf(stderr, "usage: %s rom.bin [frames] [out.ppm] [out.wav]\n", argv[0]); return 2; }
    uint32_t frames = argc > 2 ? atoi(argv[2]) : 600;
    static uint8_t rom[512 << 10];
    size_t n;
    if (load(argv[1], rom, sizeof rom, &n)) return 2;
    { int e = a26_init(&ctx, rom, n); if (e) { printf("init failed: %s\n", e == 2 ? "ARM cartridge (DPC+/CDF), not supported" : "unsupported size"); return 1; } }
    { char b[5]; mapper_name(&ctx, b); printf("mapper %s, %zu bytes\n", b, n); }
    a26_set_output(&ctx, (uint8_t *)fb, FB_W, FB_H, 16, 12);
    ctx.crc_on = getenv("NOCRC") == NULL;
    ctx.ring = ring; ctx.ring_len = sizeof ring / 2; ctx.ring_pos = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint32_t f = 0; f < frames; f++) {
        // Press reset for a few frames at 1 s, then fire now and then.
        uint32_t m = 0;
        if (f >= 60 && f < 66) m |= IN_RESET;
        if (f >= 120 && (f % 60) < 6) m |= IN_FIRE;
        a26_set_input(&ctx, m);
        if (a26_run_frames(&ctx, 1) != 1) { printf("JAM at %04x frame %u\n", ctx.PC, f); return 1; }
        if ((f + 1) % 60 == 0) printf("frame %u crc %08x lines %u visible %u-%u\n", f + 1, ctx.frame_crc, ctx.line, ctx.vis_start, ctx.vis_end);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("BENCH frames=%u crc=%08x host=%.3fs (%.0f fps)\n", frames, ctx.frame_crc, dt, frames / dt);
    uint32_t ns = ctx.nsamples < ctx.ring_len ? ctx.nsamples : ctx.ring_len;
    int peak = 0; uint32_t nz = 0;
    for (uint32_t i = 0; i < ns; i++) { if (ring[i] > peak) peak = ring[i]; nz += ring[i] != 0; }
    printf("AUDIO rate=%d samples=%u (%.1f per frame) nonzero=%u peak=%d\n", AUDIO_RATE, ctx.nsamples,
           (double)ctx.nsamples / frames, nz, peak);
    if (argc > 3) write_ppm(argv[3]);
    if (argc > 4) {
        FILE *w = fopen(argv[4], "wb"); uint32_t bytes = ns * 2, rate = AUDIO_RATE, br = rate * 2; uint16_t ch = 1, ba = 2, bits = 16;
        uint32_t riff = 36 + bytes, fmt = 16; uint16_t pcm = 1;
        fwrite("RIFF", 1, 4, w); fwrite(&riff, 4, 1, w); fwrite("WAVEfmt ", 1, 8, w); fwrite(&fmt, 4, 1, w);
        fwrite(&pcm, 2, 1, w); fwrite(&ch, 2, 1, w); fwrite(&rate, 4, 1, w); fwrite(&br, 4, 1, w); fwrite(&ba, 2, 1, w); fwrite(&bits, 2, 1, w);
        fwrite("data", 1, 4, w); fwrite(&bytes, 4, 1, w); fwrite(ring, 2, ns, w); fclose(w);
    }
    return 0;
}
