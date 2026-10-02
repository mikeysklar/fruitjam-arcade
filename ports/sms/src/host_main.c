// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Host test harness for sms.c. Build: make -f Makefile.host
//
//   host_sms zex zexdoc.com              run a CP/M Z80 exerciser, 0 if no ERROR
//   host_sms run rom frames [capture.txt] [script] [out.wav]
//                                        frame CRCs, bench, screenshot
// The system comes from the ROM extension (.sms, .gg, .sg).
// capture.txt is written in the same BEGIN_FB/base64/END_FB format code.py
// prints over serial, so tools/fb2png.py turns either into a PNG.
// script is "from-to:mask,..." with the joypad mask in hex held on frames
// [from, to), e.g. "300-306:10,600-900:18".
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define SMS_CPM_HOOK
#include "sms.c"

static uint8_t rom[4 << 20];
static ctx_t ctx;
static uint16_t fb[320 * 240];
static uint8_t cart_ram[0x8000];

// ---- CP/M stub for zexdoc/zexall -------------------------------------------

static uint8_t mem[0x10000];
static unsigned long errors;
static char tail[64];

static void cpm_putc(int c) {
    putchar(c);
    memmove(tail, tail + 1, sizeof tail - 2);
    tail[sizeof tail - 2] = (char)c;
    if (strstr(tail, "ERROR")) {
        errors++;
        memset(tail, 0, sizeof tail);
    }
}

// Port FF = BDOS call (C=2 print E, C=9 print $-string at DE), FE = exit.
static void cpm_out(sms_t *n, uint32_t port, uint32_t v) {
    (void)v;
    if (port == 0xFE) {
        n->stop = 1;
    } else if (port == 0xFF) {
        uint32_t c = n->bc & 0xFF;
        if (c == 2) {
            cpm_putc(n->de & 0xFF);
        } else if (c == 9) {
            for (uint32_t a = n->de; mem[a & 0xFFFF] != '$'; a++) {
                cpm_putc(mem[a & 0xFFFF]);
            }
        }
        fflush(stdout);
    }
}

static int zex(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror(path);
        return 2;
    }
    size_t len = fread(mem + 0x100, 1, sizeof mem - 0x100, fp);
    fclose(fp);
    sms_t *n = &ctx.sms;
    sms_init_flat(n, mem);
    const uint8_t boot[] = {0xD3, 0xFE, 0, 0, 0, 0xC3, 0x00, 0xFE};  // 0: exit, 5: JP FE00
    memcpy(mem, boot, sizeof boot);
    mem[0xFE00] = 0xD3;  // OUT (FF),A
    mem[0xFE01] = 0xFF;
    mem[0xFE02] = 0xC9;  // RET
    n->pc = 0x100;
    n->sp = 0xFE00;
    (void)len;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t total = 0;
    while (!n->stop) {
        n->cyc = 0;
        z80_run(n, 100000000);
        total += n->cyc;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("\nZEX %s: %lu errors, %llu T-states, %.1f s\n", path, errors,
        (unsigned long long)total, dt);
    return errors ? 1 : 0;
}

// ---- ROM runs -------------------------------------------------------------

static size_t load(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror(path);
        exit(2);
    }
    size_t len = fread(rom, 1, sizeof rom, fp);
    fclose(fp);
    return len;
}

static int system_of(const char *path) {
    const char *dot = strrchr(path, '.');
    if (dot && (!strcmp(dot, ".gg") || !strcmp(dot, ".GG"))) {
        return SMS_SYS_GG;
    }
    if (dot && (!strcmp(dot, ".sg") || !strcmp(dot, ".SG"))) {
        return SMS_SYS_SG;
    }
    return SMS_SYS_SMS;
}

static uint8_t script_pad(const char *s, uint32_t f) {
    uint8_t pad = 0;
    while (s && *s) {
        unsigned a, b, m;
        int used = 0;
        if (sscanf(s, "%u-%u:%x%n", &a, &b, &m, &used) != 3) {
            break;
        }
        if (f >= a && f < b) {
            pad |= m;
        }
        s += used;
        if (*s == ',') {
            s++;
        }
    }
    return pad;
}

static void write_capture(const char *path) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    FILE *o = fopen(path, "w");
    fprintf(o, "some serial noise before\nBEGIN_FB 320 240 16\n");
    const uint8_t *p = (const uint8_t *)fb;
    size_t len = sizeof fb, col = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = p[i] << 16 | (i + 1 < len ? p[i + 1] << 8 : 0) | (i + 2 < len ? p[i + 2] : 0);
        char q[4] = {b64[v >> 18], b64[(v >> 12) & 63], i + 1 < len ? b64[(v >> 6) & 63] : '=',
                     i + 2 < len ? b64[v & 63] : '='};
        fwrite(q, 1, 4, o);
        col += 4;
        if (col == 76) {
            fputc('\n', o);
            col = 0;
        }
    }
    if (col) {
        fputc('\n', o);
    }
    fprintf(o, "END_FB\nfps 60.0\n");
    fclose(o);
}

static int run(int argc, char **argv) {
    size_t len = load(argv[2]);
    uint32_t frames = atoi(argv[3]);
    int e = ctx_init(&ctx, rom, len, system_of(argv[2]));
    if (e) {
        printf("init error %d\n", e);
        return 1;
    }
    sms_t *n = &ctx.sms;
    printf("system %d mapper %d rom %u\n", n->system, n->mapper, n->rom_size);
    n->cart_ram = cart_ram;
    if (!getenv("SMS_NOFB")) {
        ctx.fb = (uint8_t *)fb;
        ctx.fb_width = 320;
        ctx.fb_height = 240;
        ctx.bpp = 16;
        ctx_layout(&ctx);
    }
    ctx.crc_on = getenv("SMS_NOCRC") == NULL;
    static int16_t ring[SMS_AUDIO_SAMPLES * 2 * 3600];
    ctx.ring = ring;
    ctx.ring_frames = SMS_AUDIO_SAMPLES * 3600;
    const char *script = argc > 5 ? argv[5] : NULL;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint32_t f = 0; f < frames; f++) {
        sms_set_pad(n, script_pad(script, f));
        ctx_run_frames(&ctx, 1);
        if ((f + 1) % 60 == 0) {
            printf("frame %u crc %08x pc %04x\n", f + 1, ctx.last_crc, n->pc);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("BENCH frames=%u crc=%08x host=%.3fs (%.0f fps, %.1f us/frame)\n",
        frames, ctx.last_crc, dt, frames / dt, dt * 1e6 / frames);
    uint32_t nsamp = (frames < 3600 ? frames : 3600) * SMS_AUDIO_SAMPLES;
    int peak = 0;
    uint32_t nz = 0;
    for (uint32_t i = 0; i < nsamp * 2; i++) {
        int v = ring[i] < 0 ? -ring[i] : ring[i];
        if (v > peak) {
            peak = v;
        }
        nz += v > 64;
    }
    if (getenv("SMS_DEBUG")) {
        printf("VDP regs:");
        for (int i = 0; i < 11; i++) {
            printf(" %02x", n->reg[i]);
        }
        printf(" status %02x height %d iff1 %d im %d halted %d irq %d pc %04x sp %04x\n", n->status,
            n->height, n->iff1, n->im, n->halted, n->irq, n->pc, n->sp);
        int nz = 0;
        for (int i = 0; i < 0x4000; i++) {
            nz += n->vram[i] != 0;
        }
        printf("vram nonzero %d, line_px[100..107] %d %d %d %d\n", nz, n->line_px[100], n->line_px[101],
            n->line_px[102], n->line_px[103]);
    }
    printf("AUDIO samples=%u loud=%u peak=%d\n", nsamp, nz / 2, peak);
    if (argc > 4 && argv[4][0]) {
        write_capture(argv[4]);
    }
    if (argc > 6 && argv[6][0]) {
        FILE *w = fopen(argv[6], "wb");
        uint32_t bytes = nsamp * 4, rate = SMS_AUDIO_RATE, br = rate * 4, riff = 36 + bytes, fmt = 16;
        uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
        fwrite("RIFF", 1, 4, w); fwrite(&riff, 4, 1, w); fwrite("WAVEfmt ", 1, 8, w); fwrite(&fmt, 4, 1, w);
        fwrite(&pcm, 2, 1, w); fwrite(&ch, 2, 1, w); fwrite(&rate, 4, 1, w); fwrite(&br, 4, 1, w);
        fwrite(&ba, 2, 1, w); fwrite(&bits, 2, 1, w);
        fwrite("data", 1, 4, w); fwrite(&bytes, 4, 1, w); fwrite(ring, 4, nsamp, w);
        fclose(w);
    }
    return 0;
}

// PSG pitch check: tone 0 at period 254 should be 3579545 / 32 / 254 = 440.4 Hz,
// noise at rate 0 should toggle often. Counts rising zero crossings over 60 frames.
static int psgtest(void) {
    static uint8_t loop[1024];
    loop[0] = 0x18;  // JR $
    loop[1] = 0xFE;
    ctx_init(&ctx, loop, sizeof loop, SMS_SYS_SMS);
    sms_t *n = &ctx.sms;
    const uint8_t w[] = {0x8E, 0x0F, 0x90};  // tone 0 period 0x0FE, volume 0 (max)
    for (unsigned i = 0; i < sizeof w; i++) {
        psg_write(n, w[i]);
    }
    uint32_t cross = 0;
    int16_t prev = 0;
    for (int f = 0; f < 60; f++) {
        sms_run_frame(n);
        for (int k = 0; k < SMS_AUDIO_SAMPLES; k++) {
            int16_t s = n->audio[k];
            if (f > 5 && prev < 0 && s >= 0) {
                cross++;
            }
            prev = s;
        }
    }
    double hz = cross / (54.0 * SMS_AUDIO_SAMPLES / SMS_AUDIO_RATE);
    printf("PSG tone0 period 254: %.1f Hz (want 440.4)\n", hz);
    return (hz > 435 && hz < 446) ? 0 : 1;
}

// Mapper checks on a synthetic 256 KB ROM whose 16 KB banks hold their number
// at offset 0x100 (plus a Codemasters checksum pair for the second run).
static int mappertest(void) {
    int fail = 0;
    for (int codies = 0; codies < 2; codies++) {
        memset(rom, 0, 0x40000);
        for (int b = 0; b < 16; b++) {
            rom[b * 0x4000 + 0x100] = b;
        }
        if (codies) {
            rom[0x7FE6] = 0x34, rom[0x7FE7] = 0x12;  // 0x1234 + 0xEDCC = 0x10000
            rom[0x7FE8] = 0xCC, rom[0x7FE9] = 0xED;
        }
        ctx_init(&ctx, rom, 0x40000, SMS_SYS_SMS);
        sms_t *n = &ctx.sms;
        n->cart_ram = cart_ram;
#define CHECK(cond) if (!(cond)) { printf("FAIL %s: %s\n", codies ? "codemasters" : "sega", #cond); fail = 1; }
        CHECK(n->mapper == (codies ? SMS_MAP_CODIES : SMS_MAP_SEGA));
        if (codies) {
            CHECK(RD(0x0100) == 0 && RD(0x4100) == 1 && RD(0x8100) == 0);
            mem_write(n, 0x8000, 5);
            mem_write(n, 0x4000, 3);
            mem_write(n, 0x0000, 7);
            CHECK(RD(0x8100) == 5 && RD(0x4100) == 3 && RD(0x0100) == 7);
            mem_write(n, 0x4000, 0x80 | 2);  // + 8 KB RAM at A000
            mem_write(n, 0xA000, 0x5A);
            CHECK(RD(0xA000) == 0x5A && RD(0x4100) == 2 && n->sram_used);
        } else {
            CHECK(RD(0x0100) == 0 && RD(0x4100) == 1 && RD(0x8100) == 2);
            mem_write(n, 0xFFFF, 9);
            mem_write(n, 0xFFFE, 4);
            mem_write(n, 0xFFFD, 6);
            CHECK(RD(0x8100) == 9 && RD(0x4100) == 4 && RD(0x0100) == 0 && RD(0x0500) == 0);
            CHECK(n->rd[1] == rom + 6 * 0x4000 + 0x400);  // 0400-3FFF follows FFFD
            CHECK(RD(0xDFFF) == 9);  // register writes also land in RAM
            mem_write(n, 0xFFFC, 0x08);
            mem_write(n, 0x8000, 0xA5);
            mem_write(n, 0xFFFC, 0x0C);
            mem_write(n, 0x8000, 0x3C);
            CHECK(RD(0x8000) == 0x3C && n->cart_ram[0] == 0xA5 && n->cart_ram[0x4000] == 0x3C && n->sram_used);
            mem_write(n, 0xFFFC, 0x00);
            CHECK(RD(0x8100) == 9);
        }
    }
    printf("MAPPER %s\n", fail ? "FAIL" : "sega + codemasters OK");
    return fail;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "mapper")) {
        return mappertest();
    }
    if (argc >= 2 && !strcmp(argv[1], "psg")) {
        return psgtest();
    }
    if (argc >= 3 && !strcmp(argv[1], "zex")) {
        return zex(argv[2]);
    }
    if (argc >= 4 && !strcmp(argv[1], "run")) {
        return run(argc, argv);
    }
    fprintf(stderr, "usage: see host_main.c\n");
    return 2;
}
