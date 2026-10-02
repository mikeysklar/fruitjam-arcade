// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Host harness for the lynx core (lynx.c built with -DHOST_TEST).
//
//   host_lynx run cart.lnx|game.o frames [fbdump.txt|-] [script] [lynxboot.img]
//
// Runs the cart for N frames into a 320x240 RGB565 framebuffer laid out as on
// the board (picture at 0,18), then prints
//   BOOT kind=hle|bs93|rom addr=$xxxx size=n rotation=r
//   BENCH frames=n us_per_frame=x crc=xxxxxxxx samples=n peak=n fps_emu=x
// crc is the CRC-32 of the last frame's 12-bit colors. With fbdump the
// framebuffer is written in code.py's serial screenshot format
// (BEGIN_FB 320 240 16 / base64 / END_FB), which tools/fb2png.py reads.
// script is "frame:mask,frame:mask,..." (mask hex, LYNX_* bits) to press
// buttons from that frame on.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t pc_hist[65536];
static uint64_t n_instr;
static int watch_pc = -1, watch_n;
#define LYNX_HOOK(l, pc) do { pc_hist[pc]++; n_instr++; if ((int)(pc) == watch_pc && watch_n < 8) { watch_n++; \
    uint16_t sp_ = (uint16_t)(l->ram[0] | l->ram[1] << 8); \
    printf("WATCH $%04X A=%02X X=%02X Y=%02X S=%02X sp=%04X [sp]=%02X %02X %02X %02X\n", pc, A, X, Y, S, sp_, \
        l->ram[sp_], l->ram[(uint16_t)(sp_ + 1)], l->ram[(uint16_t)(sp_ + 2)], l->ram[(uint16_t)(sp_ + 3)]); } } while (0)
static int spr_trace;
static int wwatch = -1, wwatch_n;
#define LYNX_WR_HOOK(a, v, pc) do { if ((int)(a) == wwatch && wwatch_n < 30) { wwatch_n++; printf("CPUWR $%04X=%02X PC=$%04X\n", a, v, pc); } } while (0)
#define LYNX_SPR_WR_HOOK(l, a, v) do { if ((int)(a) == wwatch && wwatch_n < 30) { wwatch_n++; printf("SPRWR $%04X=%X scb=%04X\n", a, v, l->scbadr); } } while (0)
#define LYNX_SPR_HOOK(l) do { if (spr_trace) printf("  spr scb=%04X ctl0=%u/%u ctl1 lit=%u depth=%u data=%04X h=%d v=%d hs=%04X vs=%04X vid=%04X\n", \
    l->scbadr, l->spr_type, l->spr_bits, l->spr_literal, l->spr_reload_depth, l->sprdline, (int16_t)l->hposstrt, \
    (int16_t)l->vposstrt, l->sprhsiz, l->sprvsiz, l->vidbas); } while (0)
#include "lynx.c"

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
        fprintf(stderr, "usage: host_lynx run cart frames [fbdump.txt|-] [script] [lynxboot.img]\n");
        return 2;
    }
    uint32_t len, blen;
    uint8_t *rom = load(argv[2], &len);
    uint8_t *boot = argc > 6 ? load(argv[6], &blen) : NULL;
    int frames = atoi(argv[3]);
    const char *dump = argc > 4 && strcmp(argv[4], "-") ? argv[4] : NULL;
    const char *script = argc > 5 ? argv[5] : "";

    static lynx_t lx;
    static uint16_t fb[320 * 240];
    static int16_t ring[2 * 4096];
    lynx_t *l = &lx;
    if (getenv("LYNX_WWATCH")) {
        wwatch = (int)strtol(getenv("LYNX_WWATCH"), NULL, 16);
    }
    if (getenv("LYNX_WATCH")) {
        watch_pc = (int)strtol(getenv("LYNX_WATCH"), NULL, 16);
    }
    int e = lynx_init(l, rom, len, boot);
    if (e) {
        printf("INIT error %d\n", e);
        return 1;
    }
    static const char *kinds[] = {"none", "hle", "bs93", "rom"};
    printf("BOOT kind=%s addr=$%04X size=%u rotation=%u page=%u\n", kinds[l->boot], l->boot_addr,
        l->boot_blocks, l->rotation, l->bank0_shift ? 1u << l->bank0_shift : 0u);
    l->fb = (uint8_t *)fb;
    l->fb_width = 320;
    l->bpp = 16;
    l->x0 = 0;
    l->y0 = 18;
    l->crc_on = 1;
    l->ring = ring;
    l->ring_frames = 4096;

    int peak = 0;
    uint32_t total_samples = 0;
    FILE *wav = getenv("LYNX_WAV") ? fopen(getenv("LYNX_WAV"), "wb") : NULL;  // raw s16le stereo
    uint32_t t0 = l->ticks;
    clock_t c0 = clock();
    for (int f = 0; f < frames; f++) {
        const char *s = script;
        while (*s) {  // latest entry at or before this frame wins
            char *end;
            long at = strtol(s, &end, 10);
            if (*end != ':') {
                break;
            }
            long mask = strtol(end + 1, &end, 16);
            if (at <= f) {
                l->joy = (uint16_t)mask;
            }
            s = *end == ',' ? end + 1 : end;
        }
        uint32_t pos0 = l->ring_pos;
        spr_trace = getenv("LYNX_SPRTRACE") && f == atoi(getenv("LYNX_SPRTRACE"));
        ctx_run_frames(l, 1);
        for (uint32_t k = 0; k < l->samples; k++) {
            uint32_t p = (pos0 + k) % 4096;
            int v = abs(ring[2 * p]);
            if (v > peak) {
                peak = v;
            }
        }
        total_samples += l->samples;
        if (wav) {
            for (uint32_t k = 0; k < l->samples; k++) {
                uint32_t p = (pos0 + k) % 4096;
                fwrite(&ring[2 * p], 2, 2, wav);
            }
        }
        if (getenv("LYNX_TRACE")) {
            printf("frame %d spr=%u PC=$%04X dispadr=%04X vidbas=%04X crc=%08x I=%u flags=%02x mask=%02x\n", f, l->spr_painted, l->PC,
                l->disp_addr, l->vidbas, l->last_crc, l->I_, l->irq_flags, l->irq_mask);
        }
        if (l->stopped) {
            printf("STOPPED err=%d PC=$%04X at frame %d\n", l->err, l->PC, f);
            break;
        }
    }
    double us = (double)(clock() - c0) * 1e6 / CLOCKS_PER_SEC / (frames ? frames : 1);
    double emu_s = (double)(l->ticks - t0) / LYNX_TICKS_PER_SEC;
    printf("BENCH frames=%u us_per_frame=%.1f crc=%08x samples=%u peak=%d fps_emu=%.2f PC=$%04X instr/frame=%llu\n",
        l->frames, us, l->last_crc, total_samples, peak, emu_s > 0 ? l->frames / emu_s : 0.0, l->PC,
        (unsigned long long)(n_instr / (frames ? frames : 1)));
    if (getenv("LYNX_DEBUG")) {
        for (int k = 0; k < 8; k++) {
            ltimer_t *t = &l->tim[k];
            printf("TIM%d bk=%u cur=%d link=%u count=%u reload=%u done=%u\n", k, t->bkup, (int)t->cur, t->linking,
                t->count, t->reload, t->done);
        }
        for (int k = 0; k < 4; k++) {
            achan_t *a = &l->aud[k];
            printf("AUD%d vol=%d out=%d bk=%u link=%u count=%u reload=%u integ=%u ws=%06x\n", k, a->vol, a->out,
                a->t.bkup, a->t.linking, a->t.count, a->t.reload, a->integrate, a->ws);
        }
        printf("DEBUG irq_mask=%02x irq_flags=%02x I=%u mapctl=%02x vec=%04x sleeping=%u wai=%u "
               "tim0 bk=%u cnt=%u cur=%d tim2 bk=%u cnt=%u link=%u dispadr=%04x dma=%u collbas=%04x colloff=%04x vidbas=%04x nocoll=%u\n",
            l->irq_mask, l->irq_flags, l->I_, l->mapctl, l->ram[0xFFFE] | l->ram[0xFFFF] << 8,
            l->sleeping, l->wai, l->tim[0].bkup, l->tim[0].count, (int)l->tim[0].cur, l->tim[2].bkup,
            l->tim[2].count, l->tim[2].linking, l->disp_addr, l->dma_enable, l->collbas, l->colloff, l->vidbas, l->sys_nocollide);
    }
    if (getenv("LYNX_PCHIST")) {  // top 12 PCs
        for (int k = 0; k < (getenv("LYNX_PCN") ? atoi(getenv("LYNX_PCN")) : 12); k++) {
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
    if (getenv("LYNX_DUMP")) {
        unsigned a = (unsigned)strtoul(getenv("LYNX_DUMP"), NULL, 16);
        unsigned rows = getenv("LYNX_DUMPN") ? (unsigned)atoi(getenv("LYNX_DUMPN")) : 4;
        for (unsigned r = 0; r < rows; r++) {
            printf("%04X:", a + r * 16);
            for (unsigned k = 0; k < 16; k++) {
                printf(" %02X", l->ram[(a + r * 16 + k) & 0xFFFF]);
            }
            printf("\n");
        }
    }
    if (dump) {
        dump_fb(dump, fb, 320, 240);
    }
    return 0;
}
