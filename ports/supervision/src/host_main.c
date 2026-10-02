// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Host harness for the supervision core (supervision.c built with -DHOST_TEST).
//
//   host_sv run game.sv frames [fbdump.txt|-] [script] [mode]
//
// Runs the cart for N frames into a 320x240 RGB565 framebuffer laid out as on
// the board (mode 0: 240x240 at 40,0), then prints
//   BENCH frames=n us_per_frame=x crc=xxxxxxxx samples=n peak=n PC=$xxxx
// crc is the CRC-32 of the last picture's shades. With fbdump the
// framebuffer is written in the launcher's serial screenshot format
// (BEGIN_FB 320 240 16 / base64 / END_FB), which tools/fb2png.py reads.
// script is "frame:mask,frame:mask,..." (mask hex, SV_* bits) to press
// buttons from that frame on.
//
// Environment: SV_WAV=file writes the sound as raw s16le stereo 22050 Hz;
// SV_TRACE=1 prints a line per frame; SV_PCHIST=1 the busiest PCs;
// SV_DUMP=addr dumps 64 bytes of the CPU address space at the end.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t pc_hist[65536];
static uint64_t n_instr;
#define SV_HOOK(s, pc) do { pc_hist[pc]++; n_instr++; } while (0)
#include "supervision.c"

static uint8_t *load(const char *path, uint32_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(n ? n : 1);
    if (fread(p, 1, n, f) != (size_t)n) {
        exit(2);
    }
    fclose(f);
    *len = (uint32_t)n;
    return p;
}

static void dump_fb(const char *path, const uint16_t *fb, int w, int h) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    FILE *f = fopen(path, "w");
    if (!f) {
        perror(path);
        exit(2);
    }
    fprintf(f, "BEGIN_FB %d %d 16\n", w, h);
    const uint8_t *p = (const uint8_t *)fb;
    size_t n = (size_t)w * h * 2;
    int col = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        char q[4] = {b64[(v >> 18) & 63], b64[(v >> 12) & 63], i + 1 < n ? b64[(v >> 6) & 63] : '=',
                     i + 2 < n ? b64[v & 63] : '='};
        fwrite(q, 1, 4, f);
        col += 4;
        if (col == 76) {
            fputc('\n', f);
            col = 0;
        }
    }
    if (col) {
        fputc('\n', f);
    }
    fprintf(f, "END_FB\n");
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 4 || strcmp(argv[1], "run")) {
        fprintf(stderr, "usage: host_sv run game.sv frames [fbdump.txt|-] [script] [mode]\n");
        return 2;
    }
    uint32_t len;
    uint8_t *rom = load(argv[2], &len);
    int frames = atoi(argv[3]);
    const char *dump = argc > 4 && strcmp(argv[4], "-") ? argv[4] : NULL;
    const char *script = argc > 5 ? argv[5] : "";
    int mode = argc > 6 ? atoi(argv[6]) : SV_MODE_SMOOTH;

    static sv_t sv;
    static uint16_t fb[320 * 240];
    static int16_t ring[2 * 4096];
    sv_t *s = &sv;
    int e = sv_init(s, rom, len);
    if (e) {
        printf("INIT error %d\n", e);
        return 1;
    }
    printf("ROM size=%u banks=%u reset=$%04X\n", s->rom_size, s->banks, s->PC);
    if (ctx_set_output(s, (uint8_t *)fb, sizeof(fb), 320, 240, 16, (uint32_t)mode)) {
        printf("bad mode\n");
        return 1;
    }
    s->crc_on = 1;
    s->ring = ring;
    s->ring_frames = 4096;

    int peak = 0;
    uint32_t total_samples = 0;
    FILE *wav = getenv("SV_WAV") ? fopen(getenv("SV_WAV"), "wb") : NULL;
    clock_t c0 = clock();
    for (int f = 0; f < frames; f++) {
        const char *sc = script;
        while (*sc) {  // latest entry at or before this frame wins
            char *end;
            long at = strtol(sc, &end, 10);
            if (*end != ':') {
                break;
            }
            long mask = strtol(end + 1, &end, 16);
            if (at <= f) {
                s->joy = (uint8_t)mask;
            }
            sc = *end == ',' ? end + 1 : end;
        }
        uint32_t pos0 = s->ring_pos;
        ctx_run_frames(s, 1);
        for (uint32_t k = 0; k < s->samples; k++) {
            uint32_t p = (pos0 + k) % 4096;
            int v = ring[2 * p] > ring[2 * p + 1] ? ring[2 * p] : ring[2 * p + 1];
            if (v > peak) {
                peak = v;
            }
        }
        total_samples += s->samples;
        if (wav) {
            for (uint32_t k = 0; k < s->samples; k++) {
                uint32_t p = (pos0 + k) % 4096;
                fwrite(&ring[2 * p], 2, 2, wav);
            }
        }
        if (getenv("SV_TRACE")) {
            printf("frame %d PC=$%04X crc=%08x ctrl=%02x I=%u irq=%u tflag=%u wai=%u scroll=%u,%u\n", f, s->PC,
                s->last_crc, s->reg[0x26], s->I_, s->irq, s->timer_flag, s->wai, s->reg[2], s->reg[3]);
        }
        if (s->stopped) {
            printf("STOPPED err=%d PC=$%04X at frame %d\n", s->err, s->PC, f);
            break;
        }
    }
    if (wav) {
        fclose(wav);
    }
    double us = (double)(clock() - c0) * 1e6 / CLOCKS_PER_SEC / (frames ? frames : 1);
    printf("BENCH frames=%u us_per_frame=%.1f crc=%08x samples=%u peak=%d PC=$%04X instr/frame=%llu\n",
        s->frames, us, s->last_crc, total_samples, peak, s->PC,
        (unsigned long long)(n_instr / (frames ? frames : 1)));
    if (getenv("SV_PCHIST")) {
        for (int k = 0; k < 12; k++) {
            uint32_t best = 0, bi = 0;
            for (uint32_t i = 0; i < 65536; i++) {
                if (pc_hist[i] > best) {
                    best = pc_hist[i];
                    bi = i;
                }
            }
            printf("PC $%04X %u\n", bi, best);
            pc_hist[bi] = 0;
        }
    }
    if (getenv("SV_DUMP")) {
        unsigned a = (unsigned)strtoul(getenv("SV_DUMP"), NULL, 16);
        for (unsigned r = 0; r < 4; r++) {
            printf("%04X:", a + r * 16);
            for (unsigned k = 0; k < 16; k++) {
                printf(" %02X", sv_peek(s, (uint16_t)(a + r * 16 + k)));
            }
            printf("\n");
        }
    }
    if (dump) {
        dump_fb(dump, fb, 320, 240);
    }
    return 0;
}
