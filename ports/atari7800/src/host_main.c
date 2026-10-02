// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Host reference for atari7800.c: runs a cartridge for N frames through the
// same framebuffer path the board uses and prints the last frame's CRC.
//
//   host_main rom.a78 frames [-o fb.txt] [-i inputs] [-w out.wav] [-8] [-t top]
//
//   -o   write the framebuffer as the serial BEGIN_FB block code.py prints
//        (turn it into a PNG with ../tools/fb2png.py)
//   -i   input script "first-last:mask,..." (frame numbers from 1, mask in
//        hex, INPUT_* bits), e.g. "100-104:40" presses reset on frames 100-104
//   -w   write the audio as a mono 16 bit WAV
//   -8   RGB332 framebuffer instead of RGB565
//   -t   first scanline shown (default 17, as in code.py)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define HOST_TEST 1
#include "atari7800.c"

#define FB_W 320
#define FB_H 240

static uint8_t fb[FB_W * FB_H * 2];
static ctx_t ctx;
static uint8_t rom[0x110000];
static int16_t ring[AUDIO_RATE * 600];

static uint32_t input_at(const char *script, uint32_t frame) {
    uint32_t m = 0;
    const char *p = script;
    while (p && *p) {
        unsigned a, b, mask;
        if (sscanf(p, "%u-%u:%x", &a, &b, &mask) == 3 && frame >= a && frame <= b) {
            m |= mask;
        }
        p = strchr(p, ',');
        if (p) p++;
    }
    return m;
}

static void write_fb(const char *path, int bpp) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    FILE *o = fopen(path, "w");
    size_t n = FB_W * FB_H * bpp / 8, col = 0;
    fprintf(o, "BEGIN_FB %d %d %d\n", FB_W, FB_H, bpp);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = fb[i] << 16;
        if (i + 1 < n) v |= fb[i + 1] << 8;
        if (i + 2 < n) v |= fb[i + 2];
        char q[4] = {b64[v >> 18], b64[(v >> 12) & 63], i + 1 < n ? b64[(v >> 6) & 63] : '=', i + 2 < n ? b64[v & 63] : '='};
        fwrite(q, 1, 4, o);
        col += 4;
        if (col == 76) { fputc('\n', o); col = 0; }
    }
    if (col) fputc('\n', o);
    fprintf(o, "END_FB\n");
    fclose(o);
}

static void write_wav(const char *path, uint32_t n) {
    FILE *o = fopen(path, "wb");
    uint32_t bytes = n * 2, rate = AUDIO_RATE, brate = AUDIO_RATE * 2, v32;
    uint16_t v16;
    fwrite("RIFF", 1, 4, o); v32 = 36 + bytes; fwrite(&v32, 4, 1, o);
    fwrite("WAVEfmt ", 1, 8, o); v32 = 16; fwrite(&v32, 4, 1, o);
    v16 = 1; fwrite(&v16, 2, 1, o); fwrite(&v16, 2, 1, o);
    fwrite(&rate, 4, 1, o); fwrite(&brate, 4, 1, o);
    v16 = 2; fwrite(&v16, 2, 1, o); v16 = 16; fwrite(&v16, 2, 1, o);
    fwrite("data", 1, 4, o); fwrite(&bytes, 4, 1, o);
    fwrite(ring, 2, n, o);
    fclose(o);
}

// --check: run the test cart from testcart/make_testcart.py with right and
// fire held, compare its pixel list, and measure the TIA test tone.
static int check_cart(const char *cart, const char *expect) {
    FILE *fp = fopen(cart, "rb");
    if (!fp) { perror(cart); return 2; }
    size_t len = fread(rom, 1, sizeof rom, fp);
    fclose(fp);
    if (a78_init(&ctx, rom, (uint32_t)len)) { printf("FAIL init\n"); return 1; }
    a78_set_output(&ctx, fb, FB_W, FB_H, 16, 17);
    ctx.ring = ring;
    ctx.ring_len = sizeof ring / 2;
    a78_set_input(&ctx, IN_RIGHT | IN_FIRE1);
    a78_run_frames(&ctx, 30);
    FILE *ex = fopen(expect, "r");
    if (!ex) { perror(expect); return 2; }
    int z, r, x, n = 0, fails = 0;
    char tok[16];
    while (fscanf(ex, "%d %d %d %15s", &z, &r, &x, tok) == 4) {
        unsigned want;
        if (!strcmp(tok, "bg")) want = 0x00;
        else if (!strcmp(tok, "swcha")) want = 0x7F;   // right held
        else if (!strcmp(tok, "inpt1")) want = 0x80;   // fire held
        else want = (unsigned)strtoul(tok, NULL, 16);
        int row = 17 + 16 * (z - 1) + r;
        uint16_t got = ((uint16_t *)fb)[row * FB_W + x];
        n++;
        if (got != ctx.pal16[want]) {
            int back = -1;
            for (int i = 0; i < 256; i++) if (ctx.pal16[i] == got) { back = i; break; }
            printf("FAIL zone %d row %d x %d: want %02x got %02x\n", z, r, x, want, back);
            fails++;
        }
    }
    fclose(ex);
    // AUDC0=4 AUDF0=15: square wave at 31538 / 32 = 985.6 Hz.
    uint32_t ns = ctx.nsamples, rises = 0;
    for (uint32_t i = 1; i < ns; i++) rises += ring[i] > 4000 && ring[i - 1] <= 4000;
    double hz = rises * (double)AUDIO_RATE / ns;
    if (hz < 985.6 * 0.98 || hz > 985.6 * 1.02) { printf("FAIL tone %.1f Hz\n", hz); fails++; }
    printf("%s: %d pixel checks, %d failed; tone %.1f Hz\n", fails ? "FAIL" : "PASS", n, fails, hz);
    return fails != 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--check")) {
        return check_cart(argv[2], argv[3]);
    }
    if (argc < 3) {
        fprintf(stderr, "usage: %s rom frames [-o fb.txt] [-i inputs] [-w out.wav] [-8] [-t top]\n", argv[0]);
        return 2;
    }
    const char *fbout = NULL, *inputs = NULL, *wav = NULL;
    int bpp = 16, top = 17;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) fbout = argv[++i];
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) inputs = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) top = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-8")) bpp = 8;
    }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 2; }
    size_t len = fread(rom, 1, sizeof rom, fp);
    fclose(fp);
    int e = a78_init(&ctx, rom, (uint32_t)len);
    if (e) {
        printf("init failed: %d (mapper %04x)\n", e, ctx.mapper);
        return 1;
    }
    a78_set_output(&ctx, fb, FB_W, FB_H, bpp, top);
    ctx.crc_on = 1;
    ctx.ring = ring;
    ctx.ring_len = sizeof ring / 2;
    uint32_t frames = atoi(argv[2]);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t done = 0;
    for (uint32_t f = 1; f <= frames; f++) {
        a78_set_input(&ctx, input_at(inputs, f));
        if (a78_run_frames(&ctx, 1) != 1) {
            printf("JAM at %04x after %u frames\n", ctx.PC, done);
            break;
        }
        done++;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double us = ((t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3) / (done ? done : 1);
    int peak = 0;
    uint32_t ns = ctx.nsamples < ctx.ring_len ? ctx.nsamples : ctx.ring_len;
    for (uint32_t i = 0; i < ns; i++) {
        if (ring[i] > peak) peak = ring[i];
    }
    printf("BENCH %s mapper=%s rom=%u pokey=%d frames=%u us/frame=%.1f crc=%08x samples=%u peak=%d\n",
           argv[1], mapper_name(&ctx), ctx.rom_len, ctx.pokey4000 ? 0x4000 : ctx.pokey450 ? 0x450 : 0,
           done, us, ctx.frame_crc, ctx.nsamples, peak);
    if (fbout) write_fb(fbout, bpp);
    if (wav) write_wav(wav, ns);
    return done == frames ? 0 : 1;
}
