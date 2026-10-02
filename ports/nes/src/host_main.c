// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Host test harness for nes.c. Build: make -f Makefile.host
//
//   host_nes nestest nestest.nes nestest.log   CPU trace vs the reference log
//   host_nes blargg rom.nes [frames]           blargg $6000 result protocol
//   host_nes run rom.nes frames [out.ppm] [out.wav] [script]
//                                              frame CRCs, bench, screenshot
// script for run: "start" presses Start on frames 300-305 and 420-425, then
// holds A+Right on alternate half seconds from frame 600.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "nes.c"

static uint8_t rom[4 << 20];
static ctx_t ctx;
static uint16_t fb[320 * 240];

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

// Compare PC, A, X, Y, P, SP and CPU cycle against every line of nestest.log.
static int nestest(const char *romp, const char *logp) {
    size_t len = load(romp);
    if (ctx_init(&ctx, rom, len)) {
        printf("init failed\n");
        return 1;
    }
    nes_t *n = &ctx.nes;
    n->pc = 0xC000;
    n->fi = 1;
    n->s = 0xFD;
    n->cyc = 7;
    FILE *lf = fopen(logp, "r");
    char line[256];
    int ln = 0;
    while (fgets(line, sizeof line, lf)) {
        ln++;
        unsigned pc, a, x, y, p, sp, cyc;
        pc = strtoul(line, NULL, 16);
        char *q = strstr(line, "A:");
        if (!q || sscanf(q, "A:%x X:%x Y:%x P:%x SP:%x", &a, &x, &y, &p, &sp) != 5) {
            continue;
        }
        cyc = strtoul(strstr(line, "CYC:") + 4, NULL, 10);
        unsigned gp = nes_get_p(n);
        if (n->pc != pc || n->a != a || n->x != x || n->y != y || gp != p || n->s != sp ||
            (unsigned)n->cyc != cyc) {
            printf("MISMATCH line %d\n  log: %s  got: %04X A:%02X X:%02X Y:%02X P:%02X SP:%02X CYC:%d\n",
                ln, line, n->pc, n->a, n->x, n->y, gp, n->s, n->cyc);
            return 1;
        }
        cpu_run(n, n->cyc + 1);
        if (n->err) {
            printf("jam at %04X line %d\n", n->err_addr, ln);
            return 1;
        }
    }
    printf("NESTEST %d lines match; $02=%02X $03=%02X\n", ln, n->ram[2], n->ram[3]);
    return (n->ram[2] || n->ram[3]) ? 1 : 0;
}

// blargg: $6000 = 0x80 while running, then the result code; text at $6004.
static int blargg(const char *romp, uint32_t frames) {
    size_t len = load(romp);
    int e = ctx_init(&ctx, rom, len);
    if (e) {
        printf("init error %d\n", e);
        return 1;
    }
    nes_t *n = &ctx.nes;
    int started = 0;
    for (uint32_t f = 0; f < frames; f++) {
        ctx_run_frames(&ctx, 1);
        if (n->err) {
            printf("jam at %04X\n", n->err_addr);
            return 1;
        }
        uint8_t *r = n->prg_ram;
        if (r[1] == 0xDE && r[2] == 0xB0 && r[3] == 0x61) {
            if (r[0] == 0x80) {
                started = 1;
            } else if (r[0] == 0x81) {
                // Needs a reset; not supported here.
                printf("wants reset\n");
                break;
            } else if (started) {
                printf("%s", (char *)r + 4);
                printf("BLARGG %s result=%d frame=%u\n", romp, r[0], f);
                return r[0];
            }
        }
    }
    printf("BLARGG %s no result after %u frames\n", romp, frames);
    return 99;
}

static int run(int argc, char **argv) {
    size_t len = load(argv[2]);
    uint32_t frames = atoi(argv[3]);
    int e = ctx_init(&ctx, rom, len);
    if (e) {
        printf("init error %d\n", e);
        return 1;
    }
    nes_t *n = &ctx.nes;
    printf("mapper %d prg %u chr %u%s battery %d\n", n->mapper, n->prg_size, n->chr_size,
        n->chr_is_ram ? " (RAM)" : "", n->battery);
    ctx.fb = getenv("NES_NOFB") ? NULL : (uint8_t *)fb;
    ctx.fb_width = 320;
    ctx.bpp = 16;
    ctx.x0 = 32;
    ctx.y0 = 0;
    ctx.crc_on = getenv("NES_NOCRC") == NULL;  // CRC costs time; off for pure benches
    static int16_t ring[NES_AUDIO_SAMPLES * 2 * 3600];
    ctx.ring = ring;
    ctx.ring_frames = NES_AUDIO_SAMPLES * 3600;
    int script = argc > 6 && !strcmp(argv[6], "start");
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint32_t f = 0; f < frames; f++) {
        uint8_t pad = 0;
        if (script && ((f >= 300 && f < 306) || (f >= 420 && f < 426))) {
            pad = NES_START;
        }
        if (script && f >= 600 && (f / 30) % 2) {
            pad |= NES_A | NES_RIGHT;
        }
        n->pad = pad;
        if (ctx_run_frames(&ctx, 1) != 1) {
            printf("jam at %04X frame %u\n", n->err_addr, f);
            return 1;
        }
        if ((f + 1) % 60 == 0) {
            printf("frame %u crc %08x\n", f + 1, ctx.last_crc);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("BENCH frames=%u crc=%08x host=%.3fs (%.0f fps, %.1f us/frame) instrs/frame=%.0f\n",
        frames, ctx.last_crc, dt, frames / dt, dt * 1e6 / frames, (double)n->instrs / frames);
    uint32_t nsamp = (frames < 3600 ? frames : 3600) * NES_AUDIO_SAMPLES;
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
    if (argc > 4 && argv[4][0]) {
        FILE *o = fopen(argv[4], "wb");
        fprintf(o, "P6 %d %d 255\n", NES_W, NES_H);
        for (int yy = 0; yy < NES_H; yy++) {
            for (int xx = 0; xx < NES_W; xx++) {
                uint16_t c = fb[yy * 320 + 32 + xx];
                fputc(((c >> 11) & 31) << 3, o);
                fputc(((c >> 5) & 63) << 2, o);
                fputc((c & 31) << 3, o);
            }
        }
        fclose(o);
    }
    if (argc > 5 && argv[5][0]) {
        FILE *w = fopen(argv[5], "wb");
        uint32_t bytes = nsamp * 4, rate = NES_AUDIO_RATE, br = rate * 4, riff = 36 + bytes, fmt = 16;
        uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
        fwrite("RIFF", 1, 4, w); fwrite(&riff, 4, 1, w); fwrite("WAVEfmt ", 1, 8, w); fwrite(&fmt, 4, 1, w);
        fwrite(&pcm, 2, 1, w); fwrite(&ch, 2, 1, w); fwrite(&rate, 4, 1, w); fwrite(&br, 4, 1, w);
        fwrite(&ba, 2, 1, w); fwrite(&bits, 2, 1, w);
        fwrite("data", 1, 4, w); fwrite(&bytes, 4, 1, w); fwrite(ring, 4, nsamp, w);
        fclose(w);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "nestest")) {
        return nestest(argv[2], argv[3]);
    }
    if (argc >= 3 && !strcmp(argv[1], "blargg")) {
        return blargg(argv[2], argc > 3 ? atoi(argv[3]) : 3000);
    }
    if (argc >= 4 && !strcmp(argv[1], "run")) {
        return run(argc, argv);
    }
    fprintf(stderr, "usage: see host_main.c\n");
    return 2;
}
