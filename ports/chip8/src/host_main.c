// Host test for chip8.c. Runs a ROM for N frames and prints the display as
// ASCII art plus its CRC. With --suite DIR it runs the Timendus chip8-test-suite
// ROMs and compares each final display CRC to the value recorded after checking
// the screen by eye (see NOTES.md).
//
//   host_test rom.ch8 [frames] [platform] [chip8|schip] [ipf] [key:from:to]
//   host_test --suite testroms
//
// platform is written to 0x1FF before start; the Timendus ROMs read it to
// skip their menus. 0 leaves memory alone.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "chip8.c"

typedef struct {
    int key, from, to;  // hold hex key during frames [from, to)
} press_t;

static uint32_t run(const char *path, uint32_t frames, int platform, uint32_t quirks,
    uint32_t ipf, const press_t *press, int show, uint32_t *beep_frames) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("cannot open %s\n", path);
        exit(2);
    }
    static uint8_t rom[MEM_SIZE];
    size_t n = fread(rom, 1, sizeof rom, fp);
    fclose(fp);
    static ctx_t ctx;
    if (c8_init(&ctx, rom, n, 1234)) {
        printf("ROM too big\n");
        exit(1);
    }
    ctx.quirks = quirks;
    ctx.ipf = ipf;
    if (platform) {
        ctx.mem[0x1FF] = (uint8_t)platform;
    }
    // Exercise the audio and RGB565 render paths too.
    static int16_t ring[AUDIO_SAMPLES * 2 * 4];
    ctx.ring = ring;
    ctx.ring_frames = AUDIO_SAMPLES * 4;
    static uint16_t fb[320 * 240];
    ctx.fb = (uint8_t *)fb;
    ctx.fb_w = 320;
    ctx.fb_h = 240;
    ctx.bpp = 16;
    uint32_t beeps = 0;
    for (uint32_t f = 0; f < frames; f++) {
        ctx.keys = 0;
        if (press && f >= (uint32_t)press->from && f < (uint32_t)press->to) {
            ctx.keys = 1u << press->key;
        }
        beeps += ctx.st > 0;
        if (c8_run_frames(&ctx, 1) != 1) {
            break;
        }
    }
    if (beep_frames) {
        *beep_frames = beeps;
    }
    if (ctx.err) {
        printf("%s: err %u at 0x%03x\n", path, ctx.err, ctx.err_addr);
    }
    uint32_t w = ctx.hires ? 128 : 64, h = ctx.hires ? 64 : 32, s = ctx.hires ? 1 : 2;
    if (show) {
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                putchar(ctx.disp[y * s * DISP_W + x * s] ? '#' : '.');
            }
            putchar('\n');
        }
    }
    // Cross-check the framebuffer against the display: sample each logical
    // pixel's center.
    uint32_t sc = 320 / w < 240 / h ? 320 / w : 240 / h;
    uint32_t x0 = (320 - w * sc) / 2, y0 = (240 - h * sc) / 2, bad = 0;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint16_t want = ctx.disp[y * s * DISP_W + x * s] ? 0xFFFF : 0;
            if (fb[(y0 + y * sc + sc / 2) * 320 + x0 + x * sc + sc / 2] != want) {
                bad++;
            }
        }
    }
    if (bad) {
        printf("%s: framebuffer mismatch in %u pixels\n", path, bad);
        exit(1);
    }
    return crc32(ctx.disp, sizeof ctx.disp);
}

static uint32_t parse_quirks(const char *s) {
    return strcmp(s, "schip") == 0 ? QUIRKS_SCHIP : QUIRKS_CHIP8;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "--suite") == 0) {
        static const struct {
            const char *rom;
            uint32_t frames;
            int platform;
            uint32_t quirks, ipf;
            press_t press;
            uint32_t crc;
        } t[] = {
            {"1-chip8-logo.ch8", 120, 0, QUIRKS_CHIP8, 11, {0, 0, 0}, 0xd31841b4},
            {"2-ibm-logo.ch8", 120, 0, QUIRKS_CHIP8, 11, {0, 0, 0}, 0xed9c36b8},
            {"3-corax+.ch8", 120, 0, QUIRKS_CHIP8, 11, {0, 0, 0}, 0x60b0dc2c},
            {"4-flags.ch8", 240, 0, QUIRKS_CHIP8, 11, {0, 0, 0}, 0xd9abb9b9},
            {"5-quirks.ch8", 600, 1, QUIRKS_CHIP8, 30, {0, 0, 0}, 0xffc8b887},
            {"5-quirks.ch8", 600, 2, QUIRKS_SCHIP, 30, {0, 0, 0}, 0xdc83c899},
            {"6-keypad.ch8", 180, 3, QUIRKS_CHIP8, 11, {0x5, 60, 90}, 0x71451ab5},
            {"8-scrolling.ch8", 300, 1, QUIRKS_SCHIP, 30, {0, 0, 0}, 0x92a0d8e2},
            {"8-scrolling.ch8", 300, 3, QUIRKS_SCHIP, 30, {0, 0, 0}, 0x29e2dd09},
        };
        int fails = 0;
        for (unsigned k = 0; k < sizeof t / sizeof t[0]; k++) {
            char path[512];
            snprintf(path, sizeof path, "%s/%s", argv[2], t[k].rom);
            const press_t *p = t[k].press.to ? &t[k].press : NULL;
            uint32_t crc = run(path, t[k].frames, t[k].platform, t[k].quirks, t[k].ipf, p,
                argc > 3, NULL);
            int ok = crc == t[k].crc;
            fails += !ok;
            printf("%-18s platform %d  crc %08x  %s\n", t[k].rom, t[k].platform, crc,
                ok ? "PASS" : "FAIL");
        }
        // 7-beep: sound timer must be running for part of the SOS pattern.
        char path[512];
        snprintf(path, sizeof path, "%s/7-beep.ch8", argv[2]);
        uint32_t beeps = 0;
        run(path, 600, 0, QUIRKS_CHIP8, 11, NULL, 0, &beeps);
        int ok = beeps > 30 && beeps < 400;
        fails += !ok;
        printf("%-18s beep frames %u  %s\n", "7-beep.ch8", beeps, ok ? "PASS" : "FAIL");
        printf("%s\n", fails ? "SUITE FAIL" : "SUITE PASS");
        return fails != 0;
    }
    if (argc < 2) {
        fprintf(stderr, "usage: %s rom.ch8 [frames] [platform] [chip8|schip] [ipf] [key:from:to]\n"
            "       %s --suite DIR [show]\n", argv[0], argv[0]);
        return 2;
    }
    uint32_t frames = argc > 2 ? atoi(argv[2]) : 300;
    int platform = argc > 3 ? atoi(argv[3]) : 0;
    uint32_t quirks = argc > 4 ? parse_quirks(argv[4]) : QUIRKS_CHIP8;
    uint32_t ipf = argc > 5 ? atoi(argv[5]) : 11;
    press_t press = {0, 0, 0};
    if (argc > 6) {
        sscanf(argv[6], "%x:%d:%d", &press.key, &press.from, &press.to);
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t crc = run(argv[1], frames, platform, quirks, ipf, argc > 6 ? &press : NULL, 1, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("frames=%u crc=%08x host=%.3fs\n", frames, crc, dt);
    return 0;
}
