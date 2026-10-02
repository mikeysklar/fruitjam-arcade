// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// The parent project's peanutgb.c under the same bench, as the calibration
// point: its speed on the Fruit Jam is measured (README: 62.6 fps emu only).
#define HOST_TEST
#include <stdint.h>
#include <stddef.h>
void *memset(void *s, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
#include "../../../src/peanutgb.c"

void *memset(void *s, int c, size_t n) {
    uint8_t *p = s;
    while (n--) { *p++ = (uint8_t)c; }
    return s;
}
void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *dp = d;
    const uint8_t *sp = s;
    while (n--) { *dp++ = *sp++; }
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    return memcpy(d, s, n);
}

static ctx_t g_ctx;

__attribute__((section(".text.entry"))) uint32_t bench_entry(uint32_t cmd, const uint8_t *rom, uint32_t arg) {
    if (cmd == 0) {
        return pgb_init(&g_ctx, rom, arg);
    }
    pgb_set_joypad(&g_ctx, arg);
    pgb_run_frames(&g_ctx, 1);
    return g_ctx.err;
}
