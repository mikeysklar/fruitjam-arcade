# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Generate an NTSC 2600 palette (128 colors) from a YUV model.

Hue 0 is grey. Hues 1-15 step around the colorburst phase starting at
gold, about 25 degrees apart. Luma has 8 levels. This is a computed
approximation, not a measured palette. Prints a C array of 0xRRGGBB.
"""
import math

SAT = 0.20
STEP = 25.0


def clamp(v):
    return max(0, min(255, int(round(v * 255))))


out = []
for c in range(128):
    hue, lum = c >> 3, c & 7
    y = 0.06 + lum * (0.88 / 7)
    if hue == 0:
        u = v = 0.0
        y = lum / 7 * 0.94
    else:
        th = math.radians(167 - (hue - 1) * STEP)
        u, v = SAT * math.cos(th), SAT * math.sin(th)
    r = y + 1.140 * v
    g = y - 0.395 * u - 0.581 * v
    b = y + 2.032 * u
    out.append((clamp(r) << 16) | (clamp(g) << 8) | clamp(b))

print("static const uint32_t ntsc_rgb[128] = {")
for i in range(0, 128, 8):
    print("    " + ", ".join("0x%06X" % x for x in out[i:i + 8]) + ",")
print("};")
