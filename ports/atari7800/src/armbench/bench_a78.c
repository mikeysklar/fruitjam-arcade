// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// atari7800.c built bare-metal for Cortex-M, driven by bench.py under Unicorn
// to count Thumb-2 instructions per emulated frame. Same flags as the .mpy.
#define HOST_TEST
#include <stdint.h>
#include <stddef.h>
void *memset(void *s, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
#include "../atari7800.c"

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
static int16_t g_ring[4 * 368 * 2];

__attribute__((section(".text.entry"))) uint32_t bench_entry(uint32_t cmd, const uint8_t *rom, uint32_t arg) {
    if (cmd == 0) {
        int e = a78_init(&g_ctx, rom, arg);
        a78_set_output(&g_ctx, (uint8_t *)g_fb, 320, 240, 16, 17);
        g_ctx.ring = g_ring;
        g_ctx.ring_len = sizeof g_ring / 2;
        return e;
    }
    a78_set_input(&g_ctx, arg);
    a78_run_frames(&g_ctx, 1);
    return g_ctx.jammed;
}
