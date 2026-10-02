// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// sms.c built bare-metal for Cortex-M, driven by bench.py under Unicorn to
// count Thumb-2 instructions per emulated frame. Same flags as the .mpy.
#define HOST_TEST
#include <stdint.h>
#include <stddef.h>
void *memset(void *s, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
#include "../sms.c"

// Word-at-a-time versions, roughly what the firmware's libc does.
void *memset(void *s, int c, size_t n) {
    uint8_t *p = s;
    uint32_t w = (uint8_t)c * 0x01010101u;
    while (n && ((uintptr_t)p & 3)) { *p++ = (uint8_t)c; n--; }
    while (n >= 4) { *(uint32_t *)p = w; p += 4; n -= 4; }
    while (n--) { *p++ = (uint8_t)c; }
    return s;
}
void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *dp = d;
    const uint8_t *sp = s;
    if ((((uintptr_t)dp | (uintptr_t)sp) & 3) == 0) {
        while (n >= 4) { *(uint32_t *)dp = *(const uint32_t *)sp; dp += 4; sp += 4; n -= 4; }
    }
    while (n--) { *dp++ = *sp++; }
    return d;
}

static ctx_t g_ctx;
static uint16_t g_fb[320 * 240];
static int16_t g_ring[SMS_AUDIO_SAMPLES * 2 * 8];

// cmd 0: init(rom, len | system << 28); cmd 1: run one frame with pad = arg.
__attribute__((section(".text.entry"))) uint32_t bench_entry(uint32_t cmd, const uint8_t *rom, uint32_t arg) {
    if (cmd == 0) {
        int e = ctx_init(&g_ctx, rom, arg & 0x0FFFFFFF, arg >> 28);
        g_ctx.fb = (uint8_t *)g_fb;
        g_ctx.fb_width = 320;
        g_ctx.fb_height = 240;
        g_ctx.bpp = 16;
        ctx_layout(&g_ctx);
        g_ctx.ring = g_ring;
        g_ctx.ring_frames = SMS_AUDIO_SAMPLES * 8;
        return e;
    }
    sms_set_pad(&g_ctx.sms, (uint8_t)arg);
    ctx_run_frames(&g_ctx, 1);
    return 0;
}
